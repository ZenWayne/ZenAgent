// tests/unit/core/event_test.cc
#include "agentflow/core/event.h"

#include <gtest/gtest.h>

namespace agentflow {
namespace {

class CapturingEmitter : public EventEmitter {
 public:
  void Emit(proto::TraceEvent ev) override { events.push_back(std::move(ev)); }
  std::vector<proto::TraceEvent> events;
};

TEST(EventTest, EmitTokenProducesTokenEvent) {
  CapturingEmitter cap;
  cap.EmitToken("llm1", "hello");
  ASSERT_EQ(cap.events.size(), 1u);
  EXPECT_EQ(cap.events[0].kind(), proto::TraceEvent::TOKEN);
  EXPECT_EQ(cap.events[0].node_id(), "llm1");
  EXPECT_EQ(cap.events[0].token().token(), "hello");
}

TEST(EventTest, EmitNodeStartEnd) {
  CapturingEmitter cap;
  cap.EmitNodeStart("planner");
  cap.EmitNodeEnd("planner", /*cancelled=*/false, /*failed=*/false);
  ASSERT_EQ(cap.events.size(), 2u);
  EXPECT_EQ(cap.events[0].kind(), proto::TraceEvent::NODE_START);
  EXPECT_EQ(cap.events[1].kind(), proto::TraceEvent::NODE_END);
  EXPECT_FALSE(cap.events[1].node_end().cancelled());
  EXPECT_FALSE(cap.events[1].node_end().failed());
}

TEST(EventTest, TimestampMonotonic) {
  CapturingEmitter cap;
  cap.EmitNodeStart("a");
  cap.EmitNodeStart("b");
  EXPECT_LE(cap.events[0].unix_micros(), cap.events[1].unix_micros());
}

}  // namespace
}  // namespace agentflow
