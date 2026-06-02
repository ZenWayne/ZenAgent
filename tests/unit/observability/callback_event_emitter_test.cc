// tests/unit/observability/callback_event_emitter_test.cc
#include "agentflow/observability/callback_event_emitter.h"

#include <vector>

#include <gtest/gtest.h>

namespace agentflow {
namespace {

TEST(CallbackEventEmitterTest, ForwardsEventsInOrder) {
  std::vector<proto::TraceEvent> seen;
  CallbackEventEmitter emit(
      [&seen](const proto::TraceEvent& e) { seen.push_back(e); });

  emit.EmitToken("a", "1");
  emit.EmitNodeStart("b");
  emit.EmitGraphDone(false);

  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0].kind(), proto::TraceEvent::TOKEN);
  EXPECT_EQ(seen[0].node_id(), "a");
  EXPECT_EQ(seen[1].kind(), proto::TraceEvent::NODE_START);
  EXPECT_EQ(seen[2].kind(), proto::TraceEvent::GRAPH_DONE);
}

TEST(CallbackEventEmitterTest, PayloadPreserved) {
  proto::TraceEvent captured;
  CallbackEventEmitter emit(
      [&captured](const proto::TraceEvent& e) { captured = e; });

  emit.EmitToolCall("agent", "search", R"({"q":"x"})");
  ASSERT_TRUE(captured.has_tool_call());
  EXPECT_EQ(captured.tool_call().tool_name(), "search");
  EXPECT_EQ(captured.tool_call().args_json(), R"({"q":"x"})");
}

}  // namespace
}  // namespace agentflow
