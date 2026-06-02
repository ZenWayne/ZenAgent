// tests/unit/observability/multi_event_emitter_test.cc
#include "agentflow/observability/multi_event_emitter.h"

#include <vector>

#include <gtest/gtest.h>

#include "agentflow/observability/callback_event_emitter.h"

namespace agentflow {
namespace {

TEST(MultiEventEmitterTest, FansOutToAllChildrenInOrder) {
  std::vector<proto::TraceEvent> a, b;
  CallbackEventEmitter ca(
      [&a](const proto::TraceEvent& e) { a.push_back(e); });
  CallbackEventEmitter cb(
      [&b](const proto::TraceEvent& e) { b.push_back(e); });
  MultiEventEmitter multi({&ca, &cb});

  multi.EmitToken("n1", "x");
  multi.EmitToken("n1", "y");
  multi.EmitGraphDone(false);

  ASSERT_EQ(a.size(), 3u);
  ASSERT_EQ(b.size(), 3u);
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].kind(), b[i].kind());
    EXPECT_EQ(a[i].node_id(), b[i].node_id());
  }
}

TEST(MultiEventEmitterTest, EmptyChildrenIsNoOp) {
  MultiEventEmitter multi({});
  EXPECT_NO_THROW(multi.EmitGraphDone(false));
}

TEST(MultiEventEmitterTest, SingleChildStillReceives) {
  std::vector<proto::TraceEvent> a;
  CallbackEventEmitter ca(
      [&a](const proto::TraceEvent& e) { a.push_back(e); });
  MultiEventEmitter multi({&ca});

  multi.EmitToken("n", "tok");
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a[0].token().token(), "tok");
}

}  // namespace
}  // namespace agentflow
