#include "agentflow/workflow/sub_agent_runtime.h"

#include <string>

#include <gtest/gtest.h>
#include <absl/status/statusor.h>
#include <asio/io_context.hpp>

#include "agentflow/core/event.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"

namespace agentflow::workflow {
namespace {

// Factory whose conversation must never be created — the gating checks
// (depth/roster) return before RunSync reaches the LLM path.
SubAgentRuntime::ConversationFactory NoLlmFactory() {
  return [](LiteRtLmConversationOptions) -> SubAgentRuntime::SendFn {
    ADD_FAILURE() << "conversation factory should not be invoked";
    return {};
  };
}

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
  SubAgentRuntime rt(wf, host_tools, emit, NoLlmFactory(), io);
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
  SubAgentRuntime rt(wf, host_tools, emit, NoLlmFactory(), io);
  SubAgentContext ctx;
  auto result = rt.RunSync("parent", "ghost", "do thing", ctx);
  ASSERT_TRUE(result.is_object());
  EXPECT_EQ(result.value("error", ""), "unknown_agent");
}

// An injected fake conversation drives the real RunSync path (no engine,
// no test-only branch inside the runtime). The fake returns a canned
// assistant message; RunSync extracts it via the default output path.
TEST(SubAgentRuntimeTest, InjectedConversationDrivesRun) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, host_tools);
  NullEventEmitter emit;

  SubAgentRuntime::ConversationFactory fake =
      [](LiteRtLmConversationOptions) -> SubAgentRuntime::SendFn {
    return [](const std::string&) -> absl::StatusOr<std::string> {
      return std::string(
          R"({"role":"assistant",)"
          R"("content":[{"type":"text","text":"pong"}]})");
    };
  };

  SubAgentRuntime rt(wf, host_tools, emit, std::move(fake), io);
  SubAgentContext ctx;
  auto result = rt.RunSync("parent", "child", "ping", ctx);
  ASSERT_TRUE(result.is_string());
  EXPECT_EQ(result.get<std::string>(), "pong");
}

// An empty SendFn from the factory means the conversation could not be
// built — RunSync surfaces engine_error rather than crashing.
TEST(SubAgentRuntimeTest, ConversationCreationFailureIsEngineError) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, host_tools);
  NullEventEmitter emit;

  SubAgentRuntime::ConversationFactory broken =
      [](LiteRtLmConversationOptions) -> SubAgentRuntime::SendFn {
    return {};
  };

  SubAgentRuntime rt(wf, host_tools, emit, std::move(broken), io);
  SubAgentContext ctx;
  auto result = rt.RunSync("parent", "child", "ping", ctx);
  ASSERT_TRUE(result.is_object());
  EXPECT_EQ(result.value("error", ""), "engine_error");
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
  SubAgentRuntime rt(wf, host_tools, emit, NoLlmFactory(), io);
  SubAgentContext ctx;
  auto result = rt.RunSync("solo", "anything", "x", ctx);
  EXPECT_EQ(result.value("error", ""), "unknown_agent");
}

}  // namespace
}  // namespace agentflow::workflow
