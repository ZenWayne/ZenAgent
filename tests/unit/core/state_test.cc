// tests/unit/core/state_test.cc
#include "agentflow/core/state.h"

#include <gtest/gtest.h>

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

TEST(StateTest, ClonePreservesData) {
  test::TestState raw;
  raw.set_counter(11);
  State s = State::From(raw);
  State copy = s.Clone();
  EXPECT_EQ(copy.As<test::TestState>().counter(), 11);
  copy.Mutable<test::TestState>().set_counter(99);
  EXPECT_EQ(s.As<test::TestState>().counter(), 11);  // original untouched
}

}  // namespace
}  // namespace agentflow
