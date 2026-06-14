// tests/unit/dynamic/schema_registry_test.cc
#include "agentflow/dynamic/schema_registry.h"

#include <memory>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <gtest/gtest.h>

#include "agentflow/core/state.h"

namespace agentflow {
namespace {

using google::protobuf::FieldDescriptor;
using google::protobuf::FieldDescriptorProto;
using google::protobuf::FileDescriptorProto;
using google::protobuf::FileDescriptorSet;

// Hand-builds a FileDescriptorSet for:
//   syntax = "proto3";
//   package dyntest;
//   message Person { string name = 1; int32 age = 2; }
// This mimics exactly what arrives over JNI from Wire's SchemaEncoder: a
// serialized descriptor set with no corresponding generated C++ type.
std::string PersonDescriptorSet() {
  FileDescriptorSet set;
  FileDescriptorProto* file = set.add_file();
  file->set_name("dyntest/person.proto");
  file->set_package("dyntest");
  file->set_syntax("proto3");

  auto* msg = file->add_message_type();
  msg->set_name("Person");

  auto* name = msg->add_field();
  name->set_name("name");
  name->set_number(1);
  name->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  name->set_type(FieldDescriptorProto::TYPE_STRING);

  auto* age = msg->add_field();
  age->set_name("age");
  age->set_number(2);
  age->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  age->set_type(FieldDescriptorProto::TYPE_INT32);

  return set.SerializeAsString();
}

TEST(SchemaRegistryTest, LoadsDescriptorAndBuildsMessageByName) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());

  auto msg = reg.NewMessage("dyntest.Person");
  ASSERT_TRUE(msg.ok()) << msg.status();
  EXPECT_EQ((*msg)->GetDescriptor()->full_name(), "dyntest.Person");
}

TEST(SchemaRegistryTest, UnknownTypeReturnsNotFound) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());

  auto msg = reg.NewMessage("dyntest.NoSuchType");
  EXPECT_FALSE(msg.ok());
  EXPECT_EQ(msg.status().code(), absl::StatusCode::kNotFound);
}

TEST(SchemaRegistryTest, MalformedDescriptorSetReturnsError) {
  SchemaRegistry reg;
  EXPECT_FALSE(reg.LoadDescriptorSet("\xff\xff not a descriptor set").ok());
}

// The core de-risk for B1: a message built from a runtime-loaded descriptor in
// one registry serializes to wire bytes that a *separately* built registry can
// parse back — proving the descriptor->DynamicMessage path is wire-correct
// across independently constructed pools (the real JNI scenario).
TEST(SchemaRegistryTest, WireRoundTripAcrossIndependentRegistries) {
  const std::string fds = PersonDescriptorSet();

  SchemaRegistry writer_reg;
  ASSERT_TRUE(writer_reg.LoadDescriptorSet(fds).ok());
  auto writer = writer_reg.NewMessage("dyntest.Person");
  ASSERT_TRUE(writer.ok()) << writer.status();

  const auto* desc = (*writer)->GetDescriptor();
  const auto* refl = (*writer)->GetReflection();
  refl->SetString(writer->get(), desc->FindFieldByName("name"), "alice");
  refl->SetInt32(writer->get(), desc->FindFieldByName("age"), 30);

  const std::string bytes = (*writer)->SerializeAsString();
  ASSERT_FALSE(bytes.empty());

  // A second, independent registry parses the same bytes.
  SchemaRegistry reader_reg;
  ASSERT_TRUE(reader_reg.LoadDescriptorSet(fds).ok());
  auto reader = reader_reg.NewMessage("dyntest.Person");
  ASSERT_TRUE(reader.ok()) << reader.status();
  ASSERT_TRUE((*reader)->ParseFromString(bytes));

  const auto* rdesc = (*reader)->GetDescriptor();
  const auto* rrefl = (*reader)->GetReflection();
  EXPECT_EQ(rrefl->GetString(**reader, rdesc->FindFieldByName("name")), "alice");
  EXPECT_EQ(rrefl->GetInt32(**reader, rdesc->FindFieldByName("age")), 30);
}

// A DynamicMessage flows through State::FromMessage and back out via
// UnsafeMessage()/reflection, just as the runner would carry it.
TEST(SchemaRegistryTest, DynamicMessageFlowsThroughState) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());
  auto msg = reg.NewMessage("dyntest.Person");
  ASSERT_TRUE(msg.ok()) << msg.status();

  const auto* desc = (*msg)->GetDescriptor();
  (*msg)->GetReflection()->SetString(msg->get(), desc->FindFieldByName("name"),
                                     "bob");

  State s = State::FromMessage(std::move(*msg));
  ASSERT_NE(s.UnsafeMessage(), nullptr);
  const auto* m = s.UnsafeMessage();
  EXPECT_EQ(m->GetReflection()->GetString(
                *m, m->GetDescriptor()->FindFieldByName("name")),
            "bob");
}

}  // namespace
}  // namespace agentflow
