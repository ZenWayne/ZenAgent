// tests/unit/dynamic/dynamic_resume_test.cc
#include "agentflow/core/runner.h"

#include <chrono>
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <gtest/gtest.h>

#include "agentflow/core/graph.h"
#include "agentflow/core/stub_node.h"
#include "agentflow/dynamic/dynamic_state.h"
#include "agentflow/dynamic/schema_registry.h"
#include "checkpoint.pb.h"

namespace agentflow {
namespace {

using namespace std::chrono_literals;

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
  return set.SerializeAsString();
}

State ResumeSync(Graph g, Runner::Options opts, const proto::Checkpoint& cp,
                 State target) {
  asio::io_context io;
  Runner runner(std::move(g), opts);
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await runner.Resume(cp, std::move(target));
      },
      asio::use_future);
  io.run();
  return fut.get();
}

// Mid-graph resume: node "a" already completed in a prior run; resuming seeds
// its successor "b" with the loaded dynamic state, and "b" runs against it.
TEST(DynamicResumeTest, ResumesMidGraphWithDynamicTargetAndMutatesViaReflection) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());

  proto::Checkpoint cp;
  cp.set_state_type("dyntest.Person");
  cp.add_completed_nodes("a");
  {
    auto seed = reg.NewMessage("dyntest.Person");
    ASSERT_TRUE(seed.ok());
    const auto* d = (*seed)->GetDescriptor();
    (*seed)->GetReflection()->SetString(seed->get(),
                                        d->FindFieldByName("name"), "alice");
    cp.set_state_bytes((*seed)->SerializeAsString());
  }

  // "b" appends "+ran" to Person.name via reflection.
  StubNode::Body append_ran = [](State& s) {
    auto* m = const_cast<google::protobuf::Message*>(s.UnsafeMessage());
    const auto* f = m->GetDescriptor()->FindFieldByName("name");
    const auto* r = m->GetReflection();
    r->SetString(m, f, r->GetString(*m, f) + "+ran");
  };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("a", 0ms, nullptr))
   .AddNode(std::make_unique<StubNode>("b", 0ms, nullptr, append_ran))
   .AddEdge("a", "b");

  auto target = MakeResumeTarget(reg, cp);
  ASSERT_TRUE(target.ok()) << target.status();

  State out = ResumeSync(b.Build(), Runner::Options{}, cp, std::move(*target));

  const auto* m = out.UnsafeMessage();
  ASSERT_NE(m, nullptr);
  const auto* f = m->GetDescriptor()->FindFieldByName("name");
  EXPECT_EQ(m->GetReflection()->GetString(*m, f), "alice+ran");
}

}  // namespace
}  // namespace agentflow
