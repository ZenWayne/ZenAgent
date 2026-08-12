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

}  // namespace
}  // namespace agentflow
