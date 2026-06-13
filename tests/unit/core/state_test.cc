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

// Cross-arm parity: ReadStringField/WriteStringField against a proto-backed
// State must round-trip a TYPE_STRING field, mirroring the JSON-arm contract
// covered in state_json_test.cc.
TEST(StateProtoTest, ReadWriteStringFieldRoundTrip) {
  State s = State::Empty<test::TestState>();
  WriteStringField(s, "last_node", "hello");

  EXPECT_EQ(ReadStringField(s, "last_node"), "hello");
  // And confirm the proto field really got set, not just an out-of-band cache.
  EXPECT_EQ(s.As<test::TestState>().last_node(), "hello");
}

// Per the header contract: read on a missing/non-string field returns "";
// write on a missing/non-string field silently noops.
TEST(StateProtoTest, ReadMissingFieldReturnsEmptyWriteNonStringNoops) {
  State s = State::Empty<test::TestState>();
  s.Mutable<test::TestState>().set_counter(7);

  // Unknown field name -> empty read.
  EXPECT_EQ(ReadStringField(s, "no_such_field"), "");
  // Non-string field (int32) -> empty read.
  EXPECT_EQ(ReadStringField(s, "counter"), "");
  // Submessage field -> empty read.
  EXPECT_EQ(ReadStringField(s, "query"), "");

  // Writes to unknown / non-string fields must be silent noops; counter must
  // remain unchanged and no exception may escape.
  WriteStringField(s, "no_such_field", "ignored");
  WriteStringField(s, "counter", "99");
  WriteStringField(s, "query", "also-ignored");
  EXPECT_EQ(s.As<test::TestState>().counter(), 7);
  EXPECT_FALSE(s.As<test::TestState>().has_query());
}

}  // namespace
}  // namespace agentflow
