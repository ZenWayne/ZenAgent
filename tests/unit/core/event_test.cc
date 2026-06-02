// tests/unit/core/event_test.cc
#include "agentflow/core/event.h"

#include <vector>

#include <gtest/gtest.h>

#include "agentflow/observability/callback_event_emitter.h"

namespace agentflow {
namespace {

// Holds a vector of captured events and a CallbackEventEmitter that pushes
// into it. `emitter` exposes the EventEmitter helpers (EmitToken, etc.).
struct EventCapture {
  std::vector<proto::TraceEvent> events;
  CallbackEventEmitter emitter{
      [this](const proto::TraceEvent& e) { events.push_back(e); }};
};

TEST(EventTest, EmitTokenProducesTokenEvent) {
  EventCapture cap;
  cap.emitter.EmitToken("llm1", "hello");
  ASSERT_EQ(cap.events.size(), 1u);
  EXPECT_EQ(cap.events[0].kind(), proto::TraceEvent::TOKEN);
  EXPECT_EQ(cap.events[0].node_id(), "llm1");
  EXPECT_EQ(cap.events[0].token().token(), "hello");
}

TEST(EventTest, EmitNodeStartEnd) {
  EventCapture cap;
  cap.emitter.EmitNodeStart("planner");
  cap.emitter.EmitNodeEnd("planner", /*cancelled=*/false, /*failed=*/false);
  ASSERT_EQ(cap.events.size(), 2u);
  EXPECT_EQ(cap.events[0].kind(), proto::TraceEvent::NODE_START);
  EXPECT_EQ(cap.events[1].kind(), proto::TraceEvent::NODE_END);
  EXPECT_FALSE(cap.events[1].node_end().cancelled());
  EXPECT_FALSE(cap.events[1].node_end().failed());
}

TEST(EventTest, EmitToolCallReturn) {
  EventCapture cap;
  cap.emitter.EmitToolCall("agent", "search", R"({"q":"x"})");
  cap.emitter.EmitToolReturn("agent", "search", R"({"hits":1})");
  ASSERT_EQ(cap.events.size(), 2u);
  EXPECT_EQ(cap.events[0].kind(), proto::TraceEvent::TOOL_CALL);
  EXPECT_EQ(cap.events[0].tool_call().tool_name(), "search");
  EXPECT_EQ(cap.events[0].tool_call().args_json(), R"({"q":"x"})");
  EXPECT_EQ(cap.events[1].kind(), proto::TraceEvent::TOOL_RETURN);
  EXPECT_EQ(cap.events[1].tool_return().result_json(), R"({"hits":1})");
}

TEST(EventTest, TimestampMonotonic) {
  EventCapture cap;
  cap.emitter.EmitNodeStart("a");
  cap.emitter.EmitNodeStart("b");
  EXPECT_LE(cap.events[0].unix_micros(), cap.events[1].unix_micros());
}

}  // namespace
}  // namespace agentflow
