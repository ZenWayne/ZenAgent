// tests/unit/inference/openai/stream_accumulator_test.cc
#include "agentflow/inference/openai/stream_accumulator.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

std::string TextFrame(const std::string& piece) {
  json f = {{"choices", json::array({{{"delta", {{"content", piece}}}}})}};
  return f.dump();
}

TEST(StreamAccumulatorTest, JoinsTextDeltasAndReportsEachOne) {
  StreamAccumulator a;
  EXPECT_EQ(a.Feed(TextFrame("He")), "He");
  EXPECT_EQ(a.Feed(TextFrame("llo")), "llo");

  EXPECT_EQ(json::parse(a.Canonical()),
            json::parse(
                R"({"role":"assistant","content":[{"type":"text","text":"Hello"}]})"));
}

TEST(StreamAccumulatorTest, RejoinsToolCallArgumentsSplitAcrossFrames) {
  // id and function.name appear ONLY in the first frame for an index; later
  // frames carry bare argument fragments.
  StreamAccumulator a;
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
         R"("function":{"name":"search","arguments":"{\"q\":"}}]}}]})");
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
         R"("function":{"arguments":"\"zen\"}"}}]}}]})");

  json got = json::parse(a.Canonical());
  ASSERT_TRUE(got.contains("tool_calls"));
  ASSERT_EQ(got["tool_calls"].size(), 1u);
  EXPECT_EQ(got["tool_calls"][0]["id"], "call_1");
  EXPECT_EQ(got["tool_calls"][0]["function"]["name"], "search");
  // Arguments stay a STRING, as OpenAI sends and AgentNode expects.
  EXPECT_EQ(got["tool_calls"][0]["function"]["arguments"], R"({"q":"zen"})");
}

TEST(StreamAccumulatorTest, MergesParallelToolCallsByIndex) {
  StreamAccumulator a;
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[)"
         R"({"index":0,"id":"c0","function":{"name":"a","arguments":"{}"}},)"
         R"({"index":1,"id":"c1","function":{"name":"b","arguments":"{"}}]}}]})");
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[)"
         R"({"index":1,"function":{"arguments":"}"}}]}}]})");

  json got = json::parse(a.Canonical());
  ASSERT_EQ(got["tool_calls"].size(), 2u);
  EXPECT_EQ(got["tool_calls"][0]["id"], "c0");
  EXPECT_EQ(got["tool_calls"][1]["id"], "c1");
  EXPECT_EQ(got["tool_calls"][1]["function"]["arguments"], "{}");
}

TEST(StreamAccumulatorTest, HandlesTextAndToolCallsInOneStream) {
  StreamAccumulator a;
  EXPECT_EQ(a.Feed(TextFrame("thinking")), "thinking");
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c",)"
         R"("function":{"name":"n","arguments":"{}"}}]}}]})");

  json got = json::parse(a.Canonical());
  EXPECT_EQ(got["content"][0]["text"], "thinking");
  EXPECT_EQ(got["tool_calls"][0]["id"], "c");
}

TEST(StreamAccumulatorTest, IgnoresRoleOnlyAndEmptyDeltaFrames) {
  StreamAccumulator a;
  EXPECT_EQ(a.Feed(R"({"choices":[{"delta":{"role":"assistant"}}]})"), "");
  EXPECT_EQ(a.Feed(R"({"choices":[{"delta":{}}]})"), "");
  EXPECT_EQ(a.Feed(R"({"choices":[{"delta":{"content":null}}]})"), "");
  EXPECT_EQ(json::parse(a.Canonical())["content"][0]["text"], "");
}

TEST(StreamAccumulatorTest, IgnoresMalformedFramesRatherThanThrowing) {
  // A provider emitting a stray keep-alive or truncated frame must not abort
  // a half-finished answer.
  StreamAccumulator a;
  EXPECT_EQ(a.Feed("not json"), "");
  EXPECT_EQ(a.Feed(R"({"no_choices":true})"), "");
  EXPECT_EQ(a.Feed(TextFrame("ok")), "ok");
  EXPECT_EQ(json::parse(a.Canonical())["content"][0]["text"], "ok");
}

TEST(StreamAccumulatorTest, SkipsNonObjectToolCallEntriesWithoutThrowing) {
  // A malformed frame can put a scalar or null inside tool_calls.
  // tc.value("index", 0) would throw type_error.306 on those.
  StreamAccumulator a;
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[null,42,)"
         R"({"index":0,"id":"c1","function":{"name":"n","arguments":"{}"}}]}}]})");

  json got = json::parse(a.Canonical());
  ASSERT_TRUE(got.contains("tool_calls"));
  // The junk entries are skipped; the real call survives intact.
  ASSERT_EQ(got["tool_calls"].size(), 1u);
  EXPECT_EQ(got["tool_calls"][0]["id"], "c1");
  EXPECT_EQ(got["tool_calls"][0]["function"]["name"], "n");
}

TEST(StreamAccumulatorTest, SkipsToolCallEntriesWithAWrongTypedIndex) {
  // A well-formed object can still carry a bad "index" — value() throws
  // type_error.302 on that, a different trigger from a non-object element
  // (is_object() alone would not catch this).
  //
  // A PRESENT but wrong-typed index must be DROPPED entirely, not defaulted
  // to 0: falling back to 0 would collide the junk entry with the genuine
  // index-0 entry that follows it in the same frame, overwriting its id and
  // name and concatenating its arguments into invalid JSON ("{}{}"). A
  // malformed frame must never corrupt a valid tool call.
  StreamAccumulator a;
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[)"
         R"({"index":null,"id":"junk","function":{"name":"n","arguments":"{}"}},)"
         R"({"index":0,"id":"c1","function":{"name":"real","arguments":"{}"}}]}}]})");

  json got = json::parse(a.Canonical());
  ASSERT_TRUE(got.contains("tool_calls"));
  // Exactly one tool call — the null-index junk entry is dropped, not
  // merged into index 0.
  ASSERT_EQ(got["tool_calls"].size(), 1u);
  EXPECT_EQ(got["tool_calls"][0]["id"], "c1");
  EXPECT_EQ(got["tool_calls"][0]["function"]["name"], "real");
  // The real entry's arguments are untouched by the dropped junk entry —
  // exactly "{}", never "{}{}". This is the assertion that proves the
  // corruption is impossible: it would fail under the old fall-back-to-0
  // behavior, which concatenated the junk entry's arguments in first.
  EXPECT_EQ(got["tool_calls"][0]["function"]["arguments"], "{}");
}

}  // namespace
}  // namespace agentflow::openai
