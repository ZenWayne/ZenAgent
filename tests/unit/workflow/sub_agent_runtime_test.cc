#include "agentflow/workflow/sub_agent_runtime.h"

#include <gtest/gtest.h>
#include <asio/io_context.hpp>

#include "agentflow/core/event.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"

namespace agentflow::workflow {
namespace {

constexpr char kRosterJson[] = R"({
  "schema_version":1,"name":"t","version":"v1",
  "state":{"kind":"dynamic_json","fields":{}},
  "agents":{
    "parent":{"system_prompt":"","model":{},"tools":[],
              "delegates":{"agents":["child"],"max_depth":2}},
    "child":{"system_prompt":"","model":{},"tools":[]}
  },
  "main":"parent"
})";

TEST(SubAgentRuntimeTest, MaxDepthEnforced) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, host_tools);

  NullEventEmitter emit;
  SubAgentRuntime rt(wf, host_tools, emit);
  SubAgentContext ctx;
  ctx.depth = 2;
  CancelSource cs;
  CancelToken tok = cs.Token();  // CancelSource::Token() returns by value
  ctx.parent_cancel = &tok;

  auto result = rt.RunSync("parent", "child", "do thing", ctx);
  ASSERT_TRUE(result.is_object());
  EXPECT_EQ(result.value("error", ""), "max_depth_exceeded");
}

TEST(SubAgentRuntimeTest, UnknownChildRejected) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, host_tools);
  NullEventEmitter emit;
  SubAgentRuntime rt(wf, host_tools, emit);
  SubAgentContext ctx;
  auto result = rt.RunSync("parent", "ghost", "do thing", ctx);
  ASSERT_TRUE(result.is_object());
  EXPECT_EQ(result.value("error", ""), "unknown_agent");
}

TEST(SubAgentRuntimeTest, SkeletonStubReturnsString) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, host_tools);
  NullEventEmitter emit;
  SubAgentRuntime rt(wf, host_tools, emit);
  SubAgentContext ctx;
  auto result = rt.RunSync("parent", "child", "hello", ctx);
  ASSERT_TRUE(result.is_string());
  EXPECT_NE(result.get<std::string>().find("child"), std::string::npos);
}

TEST(SubAgentRuntimeTest, ParentWithoutDelegatesRejected) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  // Use a workflow where main agent has NO delegates block.
  constexpr char kNoDelegate[] = R"({
    "schema_version":1,"name":"x","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{"solo":{"system_prompt":"","model":{},"tools":[]}},
    "main":"solo"
  })";
  auto wf = *WorkflowLoader::Load(kNoDelegate, host_tools);
  NullEventEmitter emit;
  SubAgentRuntime rt(wf, host_tools, emit);
  SubAgentContext ctx;
  auto result = rt.RunSync("solo", "anything", "x", ctx);
  EXPECT_EQ(result.value("error", ""), "unknown_agent");
}

}  // namespace
}  // namespace agentflow::workflow
