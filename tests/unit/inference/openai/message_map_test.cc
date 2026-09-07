// tests/unit/inference/openai/message_map_test.cc
#include "agentflow/inference/openai/message_map.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

TEST(SystemMessageTest, BareContentArrayBecomesAStringContent) {
  // ChatConversationOptions.system_message_json is a BARE array, not an object.
  auto m = SystemMessage(R"([{"type":"text","text":"You are helpful."}])");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(*m, json({{"role", "system"}, {"content", "You are helpful."}}));
}

TEST(SystemMessageTest, ConcatenatesMultipleTextItems) {
  auto m = SystemMessage(
      R"([{"type":"text","text":"a"},{"type":"text","text":"b"}])");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ((*m)["content"], "ab");
}

TEST(SystemMessageTest, EmptyOrUnparseableYieldsNothing) {
  EXPECT_FALSE(SystemMessage("").has_value());
  EXPECT_FALSE(SystemMessage("not json").has_value());
}

// FlattenContent (the shared helper behind SystemMessage and
// ToOpenAiMessages' user/assistant content) was never directly tested.
// json::value() throws type_error.306 on a non-object item and
// type_error.302 when a present "type" key has the wrong type — untrusted
// model/provider JSON must degrade to a skipped item, never an uncaught
// exception.
TEST(SystemMessageTest, SkipsNonObjectContentItemsWithoutThrowing) {
  auto m = SystemMessage(R"([42, null, "bare", [1,2],)"
                          R"({"type":"text","text":"ok"}])");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ((*m)["content"], "ok");
}

TEST(SystemMessageTest, SkipsWrongTypedTypeFieldWithoutThrowing) {
  auto m = SystemMessage(R"([{"type":42,"text":"nope"},)"
                          R"({"type":null,"text":"also nope"},)"
                          R"({"type":"text","text":"ok"}])");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ((*m)["content"], "ok");
}

TEST(SystemMessageTest, AllMalformedItemsYieldsNothing) {
  EXPECT_FALSE(SystemMessage(R"([42,{"type":7,"text":"nope"}])").has_value());
}

TEST(ToOpenAiMessagesTest, UserContentArrayFlattensToAString) {
  auto r = ToOpenAiMessages(
      R"({"role":"user","content":[{"type":"text","text":"hi"}]})");
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 1u);
  EXPECT_EQ((*r)[0], json({{"role", "user"}, {"content", "hi"}}));
}

TEST(ToOpenAiMessagesTest, AssistantWithToolCallsIsPassedThrough) {
  auto r = ToOpenAiMessages(
      R"({"role":"assistant","content":[{"type":"text","text":"let me look"}],)"
      R"("tool_calls":[{"id":"call_1","function":{"name":"s","arguments":"{}"}}]})");
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 1u);
  const json& m = (*r)[0];
  EXPECT_EQ(m["role"], "assistant");
  EXPECT_EQ(m["content"], "let me look");
  ASSERT_EQ(m["tool_calls"].size(), 1u);
  EXPECT_EQ(m["tool_calls"][0]["id"], "call_1");
  EXPECT_EQ(m["tool_calls"][0]["type"], "function");
  // arguments must stay a JSON-encoded STRING, not be parsed into an object —
  // OpenAI sends it that way and AgentNode expects that.
  ASSERT_TRUE(m["tool_calls"][0]["function"]["arguments"].is_string());
  EXPECT_EQ(m["tool_calls"][0]["function"]["arguments"], "{}");
}

TEST(ToOpenAiMessagesTest, AssistantReasoningContentIsPassedBack) {
  // DeepSeek v4 thinking mode: the previous assistant turn's
  // reasoning_content MUST be passed back on the follow-up request, else the
  // API rejects with "The reasoning_content in the thinking mode must be
  // passed back to the API". It must survive the canonical → OpenAI round-trip.
  auto r = ToOpenAiMessages(
      R"({"role":"assistant","content":[{"type":"text","text":"改"}],)"
      R"("reasoning_content":"先看项目","tool_calls":[{"id":"c1","function":{"name":"s","arguments":"{}"}}]})");
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 1u);
  const json& m = (*r)[0];
  EXPECT_EQ(m["role"], "assistant");
  EXPECT_EQ(m["reasoning_content"], "先看项目");
}

TEST(ToOpenAiMessagesTest, OneToolMessageExpandsToOnePerResult) {
  // THE reason ChatConversationOptions carries tool-call ids (design spec §3.2):
  // OpenAI needs one message per result, each with its own tool_call_id.
  auto r = ToOpenAiMessages(
      R"({"role":"tool","content":[)"
      R"({"id":"call_1","name":"search","response":{"value":"A"}},)"
      R"({"id":"call_2","name":"lookup","response":{"value":"B"}}]})");
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 2u);
  EXPECT_EQ((*r)[0], json({{"role", "tool"},
                           {"tool_call_id", "call_1"},
                           {"content", "A"}}));
  EXPECT_EQ((*r)[1], json({{"role", "tool"},
                           {"tool_call_id", "call_2"},
                           {"content", "B"}}));
}

TEST(ToOpenAiMessagesTest, ToolResultWithoutAnIdIsRejected) {
  // Better a clear error than a request OpenAI rejects with an opaque 400.
  auto r = ToOpenAiMessages(
      R"({"role":"tool","content":[{"name":"search","response":{"value":"A"}}]})");
  EXPECT_FALSE(r.ok());
}

TEST(ToOpenAiMessagesTest, SkipsNonObjectToolResultEntriesWithoutThrowing) {
  auto r = ToOpenAiMessages(
      R"({"role":"tool","content":[42,)"
      R"({"id":"call_1","name":"search","response":{"value":"A"}}]})");
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 1u);
  EXPECT_EQ((*r)[0]["tool_call_id"], "call_1");
  EXPECT_EQ((*r)[0]["content"], "A");
}

TEST(BuildRequestBodyTest, CarriesModelStreamToolsAndMessages) {
  ChatConversationOptions opts;
  opts.tools_json =
      R"([{"type":"function","function":{"name":"s","description":"d",)"
      R"("parameters":{"type":"object"}}}])";
  opts.max_output_tokens = 256;

  std::vector<json> msgs = {{{"role", "user"}, {"content", "hi"}}};
  json body = json::parse(BuildRequestBody("deepseek-chat", opts, msgs,
                                            /*stream=*/true));

  EXPECT_EQ(body["model"], "deepseek-chat");
  EXPECT_EQ(body["stream"], true);
  EXPECT_EQ(body["max_tokens"], 256);
  EXPECT_EQ(body["messages"][0]["content"], "hi");
  // BuildToolsJson already emits the OpenAI shape — passed through verbatim.
  EXPECT_EQ(body["tools"][0]["function"]["name"], "s");
}

TEST(BuildRequestBodyTest, OmitsToolsWhenThereAreNone) {
  ChatConversationOptions opts;  // tools_json defaults to "[]"
  std::vector<json> msgs = {{{"role", "user"}, {"content", "hi"}}};
  json body = json::parse(BuildRequestBody("m", opts, msgs, false));
  EXPECT_FALSE(body.contains("tools"));
  EXPECT_EQ(body["stream"], false);
}

TEST(ResponseToCanonicalTest, PlainTextAnswer) {
  auto c = ResponseToCanonical(
      R"({"choices":[{"message":{"role":"assistant","content":"42"}}]})");
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(json::parse(*c),
            json::parse(
                R"({"role":"assistant","content":[{"type":"text","text":"42"}]})"));
}

TEST(ResponseToCanonicalTest, ToolCallsArePassedThroughVerbatim) {
  auto c = ResponseToCanonical(
      R"({"choices":[{"message":{"role":"assistant","content":null,)"
      R"("tool_calls":[{"id":"call_9","type":"function",)"
      R"("function":{"name":"s","arguments":"{\"q\":1}"}}]}}]})");
  ASSERT_TRUE(c.ok());
  json got = json::parse(*c);
  ASSERT_TRUE(got.contains("tool_calls"));
  EXPECT_EQ(got["tool_calls"][0]["id"], "call_9");
  EXPECT_EQ(got["tool_calls"][0]["function"]["name"], "s");
  // arguments must stay a JSON-encoded STRING, not be parsed into an object.
  ASSERT_TRUE(got["tool_calls"][0]["function"]["arguments"].is_string());
  EXPECT_EQ(got["tool_calls"][0]["function"]["arguments"], "{\"q\":1}");
}

TEST(ResponseToCanonicalTest, MalformedOrEmptyChoicesIsAnError) {
  EXPECT_FALSE(ResponseToCanonical("not json").ok());
  EXPECT_FALSE(ResponseToCanonical(R"({"choices":[]})").ok());
}

TEST(ResponseToCanonicalTest, NonObjectChoiceIsAnErrorNotACrash) {
  EXPECT_FALSE(ResponseToCanonical(R"({"choices":[42]})").ok());
  EXPECT_FALSE(ResponseToCanonical(R"({"choices":["x"]})").ok());
  EXPECT_FALSE(ResponseToCanonical(R"({"choices":[[1,2]]})").ok());
}

TEST(ResponseToCanonicalTest, ChoiceWithoutAMessageObjectIsAnError) {
  EXPECT_FALSE(ResponseToCanonical(R"({"choices":[{}]})").ok());
  EXPECT_FALSE(ResponseToCanonical(R"({"choices":[{"message":7}]})").ok());
}

TEST(BuildRequestBodyTest, StrictToolsOnlyForDeepSeekFamily) {
  ChatConversationOptions opts;
  opts.tools_json =
      R"([{"type":"function","function":{"name":"update_dialogue","description":"d","parameters":{"type":"object","properties":{"project_id":{"type":"string"},"updates":{"type":"array","items":{"type":"object","properties":{"shot_id":{"type":"integer"}}}}},"required":["project_id","updates"]}}}])";

  auto run = [&](std::string_view model, bool strict) {
    std::string body =
        BuildRequestBody(model, opts, {}, true, strict);
    return body;
  };

  // DeepSeek family + strict: normalized (strict:true, all props required,
  // additionalProperties:false, nested empty-object untouched).
  std::string ds = run("deepseek-v4-flash", true);
  EXPECT_NE(ds.find(R"("strict":true)"), std::string::npos);
  EXPECT_NE(ds.find(R"("additionalProperties":false)"), std::string::npos);
  EXPECT_NE(ds.find(R"("required":["project_id","updates"])"), std::string::npos);

  // Non-strict: untouched (no strict:true anywhere).
  std::string ns = run("deepseek-v4-flash", false);
  EXPECT_EQ(ns.find(R"("strict":true)"), std::string::npos);

  // OTHER family + strict flag on: must NOT be normalized — strict is a
  // DeepSeek-only (Beta) feature, other OpenAI-compatible endpoints must get
  // tools verbatim.
  std::string other = run("gpt-4o", true);
  EXPECT_EQ(other.find(R"("strict":true)"), std::string::npos);
  EXPECT_EQ(other.find(R"("additionalProperties":false)"), std::string::npos);
}

TEST(BuildRequestBodyTest, EmptyObjectFallsBackNonStrict) {
  // batch_update_shots.updates[] is {"type":"object","additionalProperties":
  // true} — DeepSeek strict rejects ANY object with no properties
  // ("An object with no properties is not allowed"), even when nested. The
  // accepted strategy (verified against /beta): leave the WHOLE function
  // non-strict, schema verbatim.
  ChatConversationOptions opts;
  opts.tools_json =
      R"([{"type":"function","function":{"name":"batch_update_shots","description":"d","parameters":{"type":"object","properties":{"project_id":{"type":"string"},"updates":{"type":"array","items":{"type":"object","additionalProperties":true}}},"required":["project_id","updates"]}}}])";
  std::string body = BuildRequestBody("deepseek-v4-flash", opts, {}, true, true);
  // No strict:true anywhere; original schema passed through verbatim — the
  // empty items object keeps its original additionalProperties:true.
  EXPECT_EQ(body.find(R"("strict":true)"), std::string::npos);
  EXPECT_NE(body.find(R"("items":{"additionalProperties":true,"type":"object"})"),
            std::string::npos);
}

// --- RepairToolCallPairing ------------------------------------------------

// Builds an assistant message asking for the given tool_call ids.
json Asst(const std::vector<std::string>& ids) {
  json calls = json::array();
  for (const auto& id : ids) {
    calls.push_back({{"id", id},
                     {"type", "function"},
                     {"function", {{"name", "t"}, {"arguments", "{}"}}}});
  }
  return {{"role", "assistant"}, {"content", ""}, {"tool_calls", calls}};
}

json ToolMsg(const std::string& id) {
  return {{"role", "tool"}, {"tool_call_id", id}, {"content", "result"}};
}

// Collects, per assistant tool_calls message, the ids answered by the run of
// tool messages directly following it -- exactly what the provider checks.
std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>>
Pairing(const std::vector<json>& msgs) {
  std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> out;
  for (size_t i = 0; i < msgs.size(); ++i) {
    if (msgs[i].value("role", "") != "assistant" ||
        !msgs[i].contains("tool_calls")) {
      continue;
    }
    std::vector<std::string> want, got;
    for (const auto& c : msgs[i]["tool_calls"]) want.push_back(c.value("id", ""));
    for (size_t j = i + 1; j < msgs.size(); ++j) {
      if (msgs[j].value("role", "") != "tool") break;
      got.push_back(msgs[j].value("tool_call_id", ""));
    }
    out.push_back({want, got});
  }
  return out;
}

TEST(RepairToolCallPairingTest, FillsAnUnansweredToolCall) {
  // The poisoned-session shape: the turn ended after the model asked for a
  // tool call but before the result came back, then a new user turn followed.
  std::vector<json> msgs = {
      {{"role", "user"}, {"content", "a"}},
      Asst({"c1"}),
      {{"role", "user"}, {"content", "b"}},
  };
  RepairToolCallPairing(&msgs);

  ASSERT_EQ(msgs.size(), 4u);
  EXPECT_EQ(msgs[2]["role"], "tool");
  EXPECT_EQ(msgs[2]["tool_call_id"], "c1");
  // The placeholder must say the call did NOT run — a blank/success-looking
  // result would have the model act on data it never received.
  EXPECT_NE(msgs[2]["content"].get<std::string>().find("not executed"),
            std::string::npos);
  // The following user turn is preserved, after the inserted result.
  EXPECT_EQ(msgs[3]["role"], "user");
  EXPECT_EQ(Pairing(msgs)[0].first, Pairing(msgs)[0].second);
}

TEST(RepairToolCallPairingTest, FillsOnlyTheMissingHalfOfAPartialAnswer) {
  // "insufficient tool messages" literally: some ids answered, some not.
  std::vector<json> msgs = {Asst({"c1", "c2", "c3"}), ToolMsg("c2")};
  RepairToolCallPairing(&msgs);

  auto p = Pairing(msgs);
  ASSERT_EQ(p.size(), 1u);
  EXPECT_EQ(p[0].second, (std::vector<std::string>{"c2", "c1", "c3"}));
  // The real result is untouched; only the two absent ones are synthesized.
  EXPECT_EQ(msgs[1]["content"], "result");
}

TEST(RepairToolCallPairingTest, LeavesAFullyAnsweredHistoryAloneAndIsIdempotent) {
  const std::vector<json> original = {
      {{"role", "user"}, {"content", "a"}},
      Asst({"c1", "c2"}),
      ToolMsg("c1"),
      ToolMsg("c2"),
      {{"role", "assistant"}, {"content", "done"}},
  };
  std::vector<json> msgs = original;
  RepairToolCallPairing(&msgs);
  EXPECT_EQ(msgs, original);
  // Running it again must not start stacking placeholders.
  RepairToolCallPairing(&msgs);
  EXPECT_EQ(msgs, original);
}

TEST(RepairToolCallPairingTest, RepairsEveryAssistantTurnNotJustTheFirst) {
  std::vector<json> msgs = {
      Asst({"c1"}),
      {{"role", "user"}, {"content", "b"}},
      Asst({"c2"}),
  };
  RepairToolCallPairing(&msgs);

  auto p = Pairing(msgs);
  ASSERT_EQ(p.size(), 2u);
  EXPECT_EQ(p[0].second, (std::vector<std::string>{"c1"}));
  EXPECT_EQ(p[1].second, (std::vector<std::string>{"c2"}));
}

TEST(RepairToolCallPairingTest, IgnoresAssistantMessagesWithoutToolCalls) {
  const std::vector<json> original = {
      {{"role", "assistant"}, {"content", "just text"}},
      {{"role", "user"}, {"content", "b"}},
  };
  std::vector<json> msgs = original;
  RepairToolCallPairing(&msgs);
  EXPECT_EQ(msgs, original);
}

}  // namespace
}  // namespace agentflow::openai
