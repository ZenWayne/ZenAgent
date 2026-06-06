#include "agentflow/core/state.h"

#include <memory>

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <gtest/gtest.h>

#include "test_messages.pb.h"

namespace agentflow {
namespace {

std::shared_ptr<google::protobuf::DescriptorPool> MakePoolForTestState() {
  google::protobuf::FileDescriptorProto fdp;
  test::TestState::descriptor()->file()->CopyTo(&fdp);
  auto pool = std::make_shared<google::protobuf::DescriptorPool>();
  EXPECT_NE(pool->BuildFile(fdp), nullptr);
  return pool;
}

TEST(StateDynamicProtoTest, FromDynamicProtoReturnsProtoDynamicKind) {
  auto pool = MakePoolForTestState();
  State s = State::FromDynamicProto(pool, "agentflow.test.TestState");
  EXPECT_EQ(s.kind(), State::Kind::ProtoDynamic);
}

TEST(StateDynamicProtoTest, ReadWriteFieldRoundTrip) {
  auto pool = MakePoolForTestState();
  State s = State::FromDynamicProto(pool, "agentflow.test.TestState");
  WriteStringField(s, "user_query", "hello");
  EXPECT_EQ(ReadStringField(s, "user_query"), "hello");
}

TEST(StateDynamicProtoTest, SerializeRoundTrip) {
  auto pool = MakePoolForTestState();
  State s = State::FromDynamicProto(pool, "agentflow.test.TestState");
  WriteStringField(s, "user_query", "hello");
  std::string bytes = s.SerializeAsString();

  State t = State::FromDynamicProto(pool, "agentflow.test.TestState");
  EXPECT_TRUE(t.ParseFromString(bytes));
  EXPECT_EQ(ReadStringField(t, "user_query"), "hello");
}

TEST(StateDynamicProtoTest, UnknownMessageTypeReturnsEmpty) {
  auto pool = MakePoolForTestState();
  State s = State::FromDynamicProto(pool, "no.such.Message");
  EXPECT_TRUE(s.IsEmpty());
}

TEST(StateDynamicProtoTest, NullPoolReturnsEmpty) {
  State s = State::FromDynamicProto(nullptr, "agentflow.test.TestState");
  EXPECT_TRUE(s.IsEmpty());
}

}  // namespace
}  // namespace agentflow
