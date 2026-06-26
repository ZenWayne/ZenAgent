#include "agentflow/workflow/sub_agent_runtime.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <absl/status/statusor.h>
#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <nlohmann/json.hpp>

#include "agentflow/core/event.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"

namespace agentflow::workflow {
namespace {

// Drives the async RunAsync to completion on a local io_context and returns
// its result — RunAsync co_awaits, so tests pump the loop themselves.
nlohmann::ordered_json RunAsyncBlocking(SubAgentRuntime& rt,
                                        asio::io_context& io,
                                        std::string_view parent,
                                        std::string_view child,
                                        std::string_view goal,
                                        SubAgentContext ctx) {
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<nlohmann::ordered_json> {
        co_return co_await rt.RunAsync(parent, child, goal, ctx);
      },
      asio::use_future);
  io.run();
  io.restart();
  return fut.get();
}

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
  SubAgentRuntime rt(wf, host_tools, emit, NoLlmFactory());
  SubAgentContext ctx;
  ctx.depth = 2;
  CancelSource cs;
  CancelToken tok = cs.Token();  // CancelSource::Token() returns by value
  ctx.parent_cancel = &tok;

  auto result = RunAsyncBlocking(rt, io, "parent", "child", "do thing", ctx);
  ASSERT_TRUE(result.is_object());
  EXPECT_EQ(result.value("error", ""), "max_depth_exceeded");
}

TEST(SubAgentRuntimeTest, UnknownChildRejected) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, host_tools);
  NullEventEmitter emit;
  SubAgentRuntime rt(wf, host_tools, emit, NoLlmFactory());
  SubAgentContext ctx;
  auto result = RunAsyncBlocking(rt, io, "parent", "ghost", "do thing", ctx);
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
    return [](const std::string&, const SubAgentRuntime::TokenSink&,
              const CancelToken&)
               -> asio::awaitable<absl::StatusOr<std::string>> {
      co_return std::string(
          R"({"role":"assistant",)"
          R"("content":[{"type":"text","text":"pong"}]})");
    };
  };

  SubAgentRuntime rt(wf, host_tools, emit, std::move(fake));
  SubAgentContext ctx;
  auto result = RunAsyncBlocking(rt, io, "parent", "child", "ping", ctx);
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

  SubAgentRuntime rt(wf, host_tools, emit, std::move(broken));
  SubAgentContext ctx;
  auto result = RunAsyncBlocking(rt, io, "parent", "child", "ping", ctx);
  ASSERT_TRUE(result.is_object());
  EXPECT_EQ(result.value("error", ""), "engine_error");
}

// A streaming fake pushes deltas through the TokenSink; RunAsync forwards them
// straight to ctx.token_sink. Verifies the sub-agent → sink streaming path
// deterministically (no engine). The sink collects deltas inline as the run
// progresses — no channel, drain coroutine, or sentinel.
TEST(SubAgentRuntimeTest, StreamsDeltasToSink) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, host_tools);
  NullEventEmitter emit;

  SubAgentRuntime::ConversationFactory streaming_fake =
      [](LiteRtLmConversationOptions) -> SubAgentRuntime::SendFn {
    return [](const std::string&, const SubAgentRuntime::TokenSink& on_token,
              const CancelToken&)
               -> asio::awaitable<absl::StatusOr<std::string>> {
      if (on_token) {
        on_token("Hel");
        on_token("lo");
      }
      co_return std::string(
          R"({"role":"assistant",)"
          R"("content":[{"type":"text","text":"Hello"}]})");
    };
  };

  SubAgentRuntime rt(wf, host_tools, emit, std::move(streaming_fake));
  std::vector<std::string> got;
  SubAgentContext ctx;
  ctx.token_sink = [&got](std::string_view delta) {
    got.emplace_back(delta);
  };

  auto result = RunAsyncBlocking(rt, io, "parent", "child", "ping", ctx);

  ASSERT_TRUE(result.is_string());
  EXPECT_EQ(result.get<std::string>(), "Hello");
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], "Hel");
  EXPECT_EQ(got[1], "lo");
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
  SubAgentRuntime rt(wf, host_tools, emit, NoLlmFactory());
  SubAgentContext ctx;
  auto result = RunAsyncBlocking(rt, io, "solo", "anything", "x", ctx);
  EXPECT_EQ(result.value("error", ""), "unknown_agent");
}

}  // namespace
}  // namespace agentflow::workflow
