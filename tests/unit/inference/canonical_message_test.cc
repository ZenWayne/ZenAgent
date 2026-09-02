// tests/unit/inference/canonical_message_test.cc
#include "agentflow/inference/canonical_message.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace agentflow {
namespace {

using json = nlohmann::json;

TEST(ExtractAssistantTextTest, ConcatenatesAllTextItems) {
  EXPECT_EQ(ExtractAssistantText(
                R"({"role":"assistant","content":[{"type":"text","text":"ab"},)"
                R"({"type":"text","text":"cd"}]})"),
            "abcd");
}

TEST(ExtractAssistantTextTest, IgnoresNonTextItemsAndMissingContent) {
  EXPECT_EQ(ExtractAssistantText(
                R"({"role":"assistant","content":[{"type":"image"},)"
                R"({"type":"text","text":"ok"}]})"),
            "ok");
  EXPECT_EQ(ExtractAssistantText(R"({"role":"assistant"})"), "");
  EXPECT_EQ(ExtractAssistantText("not json at all"), "");
}

TEST(ExtractAssistantTextTest, SkipsNonObjectContentItemsWithoutThrowing) {
  // Untrusted model output may put scalars in the content array.
  EXPECT_EQ(ExtractAssistantText(
                R"({"role":"assistant","content":[42,{"type":"text","text":"ok"}]})"),
            "ok");
  EXPECT_EQ(ExtractAssistantText(
                R"({"role":"assistant","content":[null,"bare",[1,2],)"
                R"({"type":"text","text":"good"}]})"),
            "good");
  EXPECT_EQ(ExtractAssistantText(R"({"role":"assistant","content":[7]})"), "");
}

TEST(ExtractAssistantTextTest, SkipsWrongTypedTypeFieldWithoutThrowing) {
  // is_object() alone is not enough: item.value("type","") still throws
  // type_error.302 when "type" is present with the wrong type (e.g. a
  // number). A malformed item must be skipped, not crash the process.
  EXPECT_EQ(ExtractAssistantText(
                R"({"role":"assistant","content":[{"type":42,"text":"nope"},)"
                R"({"type":"text","text":"ok"}]})"),
            "ok");
  EXPECT_EQ(ExtractAssistantText(
                R"({"role":"assistant","content":[{"type":null,"text":"nope"}]})"),
            "");
}

TEST(LiteRtStreamAssemblerTest, AccumulatesTextEnvelopesIntoCanonical) {
  LiteRtStreamAssembler a;
  a.Feed(R"({"role":"assistant","content":[{"type":"text","text":"He"}]})");
  a.Feed(R"({"role":"assistant","content":[{"type":"text","text":"llo"}]})");

  EXPECT_EQ(a.text_deltas(), (std::vector<std::string>{"He", "llo"}));
  EXPECT_EQ(json::parse(a.Canonical()),
            json::parse(
                R"({"role":"assistant","content":[{"type":"text","text":"Hello"}]})"));
}

TEST(LiteRtStreamAssemblerTest, ToolCallEnvelopeWinsOverAccumulatedText) {
  // LiteRT-LM emits a complete tool_calls message as its own chunk. When one
  // arrives it IS the turn's response; earlier text is not the final answer.
  LiteRtStreamAssembler a;
  a.Feed(R"({"role":"assistant","content":[{"type":"text","text":"thinking"}]})");
  a.Feed(R"({"role":"assistant","tool_calls":[{"id":"call_1",)"
         R"("function":{"name":"search","arguments":"{\"q\":\"x\"}"}}]})");

  json got = json::parse(a.Canonical());
  ASSERT_TRUE(got.contains("tool_calls"));
  EXPECT_EQ(got["tool_calls"][0]["id"], "call_1");
  EXPECT_EQ(got["tool_calls"][0]["function"]["name"], "search");
}

TEST(LiteRtStreamAssemblerTest, NonJsonChunkIsTreatedAsRawTextDelta) {
  LiteRtStreamAssembler a;
  a.Feed("plain");
  a.Feed(" text");

  EXPECT_EQ(a.text_deltas(), (std::vector<std::string>{"plain", " text"}));
  EXPECT_EQ(ExtractAssistantText(a.Canonical()), "plain text");
}

TEST(LiteRtStreamAssemblerTest, EmptyStreamYieldsEmptyAssistantMessage) {
  LiteRtStreamAssembler a;
  json got = json::parse(a.Canonical());
  EXPECT_EQ(got["role"], "assistant");
  EXPECT_EQ(ExtractAssistantText(a.Canonical()), "");
}

TEST(LiteRtStreamAssemblerTest, MalformedContentItemInAStreamChunkDoesNotThrow) {
  // Feed() reaches ExtractAssistantText on the text-delta path, so the same
  // hazard arrives over the wire.
  LiteRtStreamAssembler a;
  a.Feed(R"({"role":"assistant","content":[42]})");
  a.Feed(R"({"role":"assistant","content":[{"type":"text","text":"hi"}]})");
  EXPECT_EQ(ExtractAssistantText(a.Canonical()), "hi");
}

TEST(DecodeSpecialTokensTest, StripsRegisteredTokensFromToolArguments) {
  // gemma4's unconstrained path leaks its registered quote token <|"|>
  // literally inside tool-call argument strings, so the delegate agent name
  // arrives as <|"|>searcher<|"|> instead of "searcher". The token set comes
  // from the probe (tokenizer-verified), never hardcoded here.
  ModelSpecialTokens tokens;
  tokens.strip = {"<|\"|>"};
  const std::string raw =
      R"({"role":"assistant","tool_calls":[{"id":"c1","function":)"
      R"({"name":"delegate","arguments":"{\"agent\":\"<|\"|>searcher<|\"|>\"}"}}]})";
  const std::string decoded = DecodeSpecialTokens(raw, tokens);

  json parsed = json::parse(decoded, nullptr, /*allow_exceptions=*/false);
  ASSERT_FALSE(parsed.is_discarded()) << decoded;
  const std::string args = parsed["tool_calls"][0]["function"]["arguments"];
  json a = json::parse(args, nullptr, /*allow_exceptions=*/false);
  ASSERT_FALSE(a.is_discarded()) << args;
  EXPECT_EQ(a["agent"], "searcher");
}

TEST(DecodeSpecialTokensTest, StripsFromTextContentToo) {
  // The same leak can appear outside tool calls (final answer text); the
  // backend-level decode must cover the whole canonical message, not just
  // arguments. The engine embeds the token text JSON-escaped (<|\"|>), which
  // is what the canonical JSON actually contains.
  ModelSpecialTokens tokens;
  tokens.strip = {"<|\"|>", "<end_of_turn>"};
  const std::string raw =
      R"({"role":"assistant","content":[{"type":"text",)"
      R"("text":"answer <|\"|>quoted<|\"|> <end_of_turn> done"}]})";
  const std::string decoded = DecodeSpecialTokens(raw, tokens);

  json parsed = json::parse(decoded, nullptr, /*allow_exceptions=*/false);
  ASSERT_FALSE(parsed.is_discarded()) << decoded;
  EXPECT_EQ(parsed["content"][0]["text"], "answer quoted  done");
}

TEST(DecodeSpecialTokensTest, EmptyTokenSetReturnsInputUnchanged) {
  ModelSpecialTokens tokens;  // empty — unknown model family
  const std::string clean =
      R"({"role":"assistant","content":[{"type":"text","text":"hi \"there\""}]})";
  EXPECT_EQ(DecodeSpecialTokens(clean, tokens), clean);
}

TEST(DecodeSpecialTokensTest, HandlesMultipleTokensAndEmptyInput) {
  ModelSpecialTokens tokens;
  tokens.strip = {"<|\"|>", "<end_of_turn>"};
  const std::string raw =
      R"({"role":"assistant","tool_calls":[{"id":"c1","function":)"
      R"({"name":"t","arguments":"{\"a\":\"<|\"|>x<|\"|>\",\"b\":\"<end_of_turn><|\"|>y<|\"|>\"}"}}]})";
  const std::string decoded = DecodeSpecialTokens(raw, tokens);
  json parsed = json::parse(decoded, nullptr, /*allow_exceptions=*/false);
  ASSERT_FALSE(parsed.is_discarded()) << decoded;
  const std::string args =
      parsed["tool_calls"][0]["function"]["arguments"].get<std::string>();
  json a = json::parse(args);
  EXPECT_EQ(a["a"], "x");
  EXPECT_EQ(a["b"], "y");
  EXPECT_EQ(DecodeSpecialTokens("", tokens), "");
}

}  // namespace
}  // namespace agentflow
