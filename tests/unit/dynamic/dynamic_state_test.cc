// tests/unit/dynamic/dynamic_state_test.cc
#include "agentflow/dynamic/dynamic_state.h"

#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <gtest/gtest.h>

#include "agentflow/dynamic/schema_registry.h"
#include "checkpoint.pb.h"

namespace agentflow {
namespace {

// syntax="proto3"; package dyntest; message Person { string name = 1; int32 age = 2; }
std::string PersonDescriptorSet() {
  google::protobuf::FileDescriptorSet set;
  auto* file = set.add_file();
  file->set_name("dyntest/person.proto");
  file->set_package("dyntest");
  file->set_syntax("proto3");
  auto* msg = file->add_message_type();
  msg->set_name("Person");
  auto* name = msg->add_field();
  name->set_name("name");
  name->set_number(1);
  name->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  name->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);
  auto* age = msg->add_field();
  age->set_name("age");
  age->set_number(2);
  age->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  age->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT32);
  return set.SerializeAsString();
}

// Build serialized Person{name=..,age=..} bytes via the registry itself.
std::string PersonBytes(SchemaRegistry& reg, const std::string& name, int age) {
  auto m = reg.NewMessage("dyntest.Person");
  if (!m.ok()) {
    ADD_FAILURE() << "PersonBytes: NewMessage failed: " << m.status();
    return {};
  }
  const auto* d = (*m)->GetDescriptor();
  const auto* r = (*m)->GetReflection();
  r->SetString(m->get(), d->FindFieldByName("name"), name);
  r->SetInt32(m->get(), d->FindFieldByName("age"), age);
  return (*m)->SerializeAsString();
}

TEST(MakeResumeTargetTest, BuildsStateWhoseTypeMatchesCheckpoint) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());

  proto::Checkpoint cp;
  cp.set_state_type("dyntest.Person");
  // state_bytes is not consumed by MakeResumeTarget (it only builds an empty
  // State of state_type); set here to show a realistic Checkpoint shape.
  cp.set_state_bytes(PersonBytes(reg, "alice", 30));

  auto target = MakeResumeTarget(reg, cp);
  ASSERT_TRUE(target.ok()) << target.status();
  ASSERT_NE(target->UnsafeMessage(), nullptr);
  EXPECT_EQ(target->UnsafeMessage()->GetTypeName(), "dyntest.Person");
}

TEST(MakeResumeTargetTest, UnknownTypeReturnsNotFound) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());

  proto::Checkpoint cp;
  cp.set_state_type("dyntest.Ghost");

  auto target = MakeResumeTarget(reg, cp);
  EXPECT_FALSE(target.ok());
  EXPECT_EQ(target.status().code(), absl::StatusCode::kNotFound);
}

}  // namespace
}  // namespace agentflow
