// tests/unit/core/state_test.cc
#include "agentflow/core/state.h"

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "test_messages.pb.h"

namespace agentflow {
namespace {

TEST(StateTest, FromAndAsRoundTrip) {
  test::TestState raw;
  raw.mutable_query()->set_text("hello");
  raw.set_counter(7);

  State s = State::From(raw);
  const auto& roundtrip = s.As<test::TestState>();
  EXPECT_EQ(roundtrip.query().text(), "hello");
  EXPECT_EQ(roundtrip.counter(), 7);
}

TEST(StateTest, MutableAllowsInPlaceUpdate) {
  test::TestState raw;
  State s = State::From(raw);

  s.Mutable<test::TestState>().set_counter(42);
  EXPECT_EQ(s.As<test::TestState>().counter(), 42);
}

TEST(StateTest, SerializeRoundTrip) {
  test::TestState raw;
  raw.mutable_query()->set_text("ping");
  State s = State::From(raw);

  std::string bytes = s.SerializeAsString();
  EXPECT_FALSE(bytes.empty());

  // Build a new State of the same type and parse:
  State s2 = State::Empty<test::TestState>();
  ASSERT_TRUE(s2.ParseFromString(bytes));
  EXPECT_EQ(s2.As<test::TestState>().query().text(), "ping");
}

TEST(StateTest, AsWrongTypeThrows) {
  test::TestState raw;
  State s = State::From(raw);
  EXPECT_THROW((void)s.As<test::UserQuery>(), AgentflowError);
}

TEST(StateTest, FromMessageWrapsExistingMessage) {
  // Simulates the dynamic-schema path: a message built elsewhere (e.g. a
  // DynamicMessage from a runtime-loaded descriptor) handed to State as a
  // base-class unique_ptr, with no compile-time knowledge of the concrete type.
  std::unique_ptr<google::protobuf::Message> msg =
      std::make_unique<test::TestState>();
  static_cast<test::TestState*>(msg.get())->set_counter(5);
  static_cast<test::TestState*>(msg.get())->mutable_query()->set_text("hi");

  State s = State::FromMessage(std::move(msg));

  ASSERT_NE(s.UnsafeMessage(), nullptr);
  EXPECT_EQ(s.As<test::TestState>().counter(), 5);
  EXPECT_EQ(s.As<test::TestState>().query().text(), "hi");
}

TEST(StateTest, ClonePreservesData) {
  test::TestState raw;
  raw.set_counter(11);
  State s = State::From(raw);
  State copy = s.Clone();
  EXPECT_EQ(copy.As<test::TestState>().counter(), 11);
  copy.Mutable<test::TestState>().set_counter(99);
  EXPECT_EQ(s.As<test::TestState>().counter(), 11);  // original untouched
}

TEST(StateTest, ParseFromStringBoundedRejectsOversize) {
  test::TestState raw;
  raw.set_user_query(std::string(1024, 'x'));  // ~1KB field
  State filled = State::From(raw);
  const std::string big = filled.SerializeAsString();
  State s = State::Empty<test::TestState>();
  absl::Status st = s.ParseFromStringBounded(big, /*max_depth=*/100,
                                             /*max_bytes=*/64);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), absl::StatusCode::kInvalidArgument);
}

TEST(StateTest, ParseFromStringBoundedAcceptsUnderLimit) {
  test::TestState raw;
  raw.set_counter(5);
  const std::string bytes = State::From(raw).SerializeAsString();
  State s = State::Empty<test::TestState>();
  ASSERT_TRUE(s.ParseFromStringBounded(bytes, 100, 1 << 20).ok());
  EXPECT_EQ(s.As<test::TestState>().counter(), 5);
}

TEST(StateTest, ParseFromStringBoundedRejectsEmptyState) {
  State s;  // default-constructed: no underlying message
  absl::Status st = s.ParseFromStringBounded("anything");
  EXPECT_EQ(st.code(), absl::StatusCode::kFailedPrecondition);
}

}  // namespace
}  // namespace agentflow
