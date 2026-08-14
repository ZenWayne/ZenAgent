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

}  // namespace
}  // namespace agentflow::openai
