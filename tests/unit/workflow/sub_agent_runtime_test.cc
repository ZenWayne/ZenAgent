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

#include "agentflow/core/errors.h"
#include "agentflow/core/event.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "tests/support/fake_chat_backend.h"

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
  return [](std::string_view, std::string_view,
            ChatConversationOptions) -> SubAgentRuntime::SendFn {
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
      [](std::string_view, std::string_view,
         ChatConversationOptions) -> SubAgentRuntime::SendFn {
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
      [](std::string_view, std::string_view,
         ChatConversationOptions) -> SubAgentRuntime::SendFn {
    return {};
  };

  SubAgentRuntime rt(wf, host_tools, emit, std::move(broken));
  SubAgentContext ctx;
  auto result = RunAsyncBlocking(rt, io, "parent", "child", "ping", ctx);
  ASSERT_TRUE(result.is_object());
  EXPECT_EQ(result.value("error", ""), "engine_error");
}

// A streaming fake pushes deltas through the TokenSink; RunAsync forwards them
// onto ctx.token_channel. Verifies the sub-agent → channel streaming path
// deterministically (no engine). Uses an empty-string sentinel to end the
// drain after RunAsync, mirroring the delegate tool.
TEST(SubAgentRuntimeTest, StreamsDeltasToChannel) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, host_tools);
  NullEventEmitter emit;

  SubAgentRuntime::ConversationFactory streaming_fake =
      [](std::string_view, std::string_view,
         ChatConversationOptions) -> SubAgentRuntime::SendFn {
    return [](const std::string&, const SubAgentRuntime::TokenSink& on_token,
              const CancelToken&)
               -> asio::awaitable<absl::StatusOr<std::string>> {
      if (on_token) {
        co_await on_token("Hel");
        co_await on_token("lo");
      }
      co_return std::string(
          R"({"role":"assistant",)"
          R"("content":[{"type":"text","text":"Hello"}]})");
    };
  };

  SubAgentRuntime rt(wf, host_tools, emit, std::move(streaming_fake));
  TokenChannel ch(io, /*capacity=*/16);
  SubAgentContext ctx;
  ctx.token_channel = &ch;

  nlohmann::ordered_json result;
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    result = co_await rt.RunAsync("parent", "child", "ping", ctx);
    auto [ec] = co_await ch.async_send(asio::error_code{}, std::string{},
                                       asio::as_tuple(asio::use_awaitable));
    (void)ec;
  }, asio::detached);

  std::vector<std::string> got;
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    for (;;) {
      auto [ec, tok] =
          co_await ch.async_receive(asio::as_tuple(asio::use_awaitable));
      if (ec || tok.empty()) break;
      got.push_back(tok);
    }
    co_return;
  }, asio::detached);

  io.run();

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

// THE DECISIVE TEST (final-review fix wave): nothing anywhere previously
// exercised a sub-agent running on a remote backend, which is how three
// defects survived 13 per-task reviews — SubAgentRuntime built a
// {role,content} system message object instead of AgentNode's bare content
// array (silently drops the system prompt on any remote backend, since
// openai::FlattenContent returns {} for an object), had no guard at all on
// the tool_calls loop, and never consulted the child's own model.backend.
// This test drives SubAgentRuntime through its PRODUCTION factory
// (DefaultConversationFactory), the same one workflow_runner.cc wires up,
// with FakeChatBackend standing in for "some backend, on-device or remote" —
// exactly the seam IChatBackend abstracts over.
constexpr char kRemoteBackendRosterJson[] = R"({
  "schema_version":1,"name":"t","version":"v1",
  "state":{"kind":"dynamic_json","fields":{}},
  "agents":{
    "parent":{"system_prompt":"","model":{},"tools":[],
              "delegates":{"agents":["child"],"max_depth":2}},
    "child":{"system_prompt":"You are a careful child agent.",
             "model":{},"tools":[]}
  },
  "main":"parent"
})";

TEST(SubAgentRuntimeTest,
     RemoteBackendGetsBareArraySystemPromptAndSurvivesMalformedToolCalls) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf = *WorkflowLoader::Load(kRemoteBackendRosterJson, host_tools);
  NullEventEmitter emit;

  // First turn: a tool_calls array containing a bare scalar — the exact
  // shape that used to throw an uncaught type_error.306 and abort the
  // process. Second turn: plain text, so the loop terminates.
  auto backend = std::make_shared<agentflow::testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant",)"
          R"("content":[{"type":"text","text":"thinking"}],)"
          R"("tool_calls":[42]})",
          R"({"role":"assistant",)"
          R"("content":[{"type":"text","text":"done"}]})"});

  SubAgentRuntime rt(wf, host_tools, emit,
                      SubAgentRuntime::DefaultConversationFactory(backend));
  SubAgentContext ctx;
  auto result = RunAsyncBlocking(rt, io, "parent", "child", "ping", ctx);

  // Catches FIX 2: a malformed tool_calls entry must not abort the run.
  ASSERT_TRUE(result.is_string());
  EXPECT_EQ(result.get<std::string>(), "done");

  // Catches FIX 1: the system prompt must reach the backend as a BARE
  // content array, not a {role,content} object — the shape
  // openai::SystemMessage()/FlattenContent actually understands.
  nlohmann::json sys = nlohmann::json::parse(
      backend->last_options().system_message_json, nullptr, false);
  ASSERT_FALSE(sys.is_discarded());
  ASSERT_TRUE(sys.is_array());
  ASSERT_EQ(sys.size(), 1u);
  EXPECT_EQ(sys[0]["type"], "text");
  EXPECT_EQ(sys[0]["text"], "You are a careful child agent.");
}

TEST(SubAgentRuntimeTest, ChildWithOwnModelBackendUsesThatBackendNotTheParents) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  constexpr char kWf[] = R"({
    "schema_version":1,"name":"t","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{
      "parent":{"system_prompt":"","model":{},"tools":[],
                "delegates":{"agents":["child"],"max_depth":2}},
      "child":{"system_prompt":"","model":{"backend":"cloud-child"},"tools":[]}
    },
    "main":"parent"
  })";
  auto wf = *WorkflowLoader::Load(kWf, host_tools);
  NullEventEmitter emit;

  auto parent_backend = std::make_shared<agentflow::testing::FakeChatBackend>(
      std::vector<std::string>{});
  auto child_backend = std::make_shared<agentflow::testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","content":[{"type":"text","text":"ok"}]})"});

  std::map<std::string, std::shared_ptr<::agentflow::IChatBackend>> backends =
      {{"cloud-child", child_backend}};

  // parent_backend plays the role of the PARENT's resolved backend (what
  // workflow_runner.cc used to hand to every child regardless of the
  // child's own model.backend); it must never be touched by this run.
  SubAgentRuntime rt(wf, host_tools, emit,
                      SubAgentRuntime::DefaultConversationFactory(
                          parent_backend, backends));
  SubAgentContext ctx;
  auto result = RunAsyncBlocking(rt, io, "parent", "child", "ping", ctx);

  ASSERT_TRUE(result.is_string());
  EXPECT_EQ(result.get<std::string>(), "ok");
  // Catches FIX 4: the child's OWN named backend was used...
  EXPECT_NE(child_backend->last_conversation(), nullptr);
  // ...and the parent's resolved backend was never touched.
  EXPECT_EQ(parent_backend->last_conversation(), nullptr);
}

// RunAsync's contract is "NEVER throws" (sub_agent_runtime.h). An
// unregistered child backend must still FAIL LOUDLY — never silently fall
// back to the parent's or host's default backend — but it must do so as a
// structured {"error":...} result (like every sibling failure in RunAsync),
// not by letting ResolveNamedBackend's AgentflowError escape uncaught.
TEST(SubAgentRuntimeTest,
     ChildWithUnregisteredBackendReturnsStructuredErrorRatherThanFallingBack) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  constexpr char kWf[] = R"({
    "schema_version":1,"name":"t","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{
      "parent":{"system_prompt":"","model":{},"tools":[],
                "delegates":{"agents":["child"],"max_depth":2}},
      "child":{"system_prompt":"","model":{"backend":"ghost"},"tools":[]}
    },
    "main":"parent"
  })";
  auto wf = *WorkflowLoader::Load(kWf, host_tools);
  NullEventEmitter emit;

  // A default IS available — the point is it must NOT be used silently.
  auto default_backend = std::make_shared<agentflow::testing::FakeChatBackend>(
      std::vector<std::string>{});
  SubAgentRuntime rt(wf, host_tools, emit,
                      SubAgentRuntime::DefaultConversationFactory(
                          default_backend, {}));
  SubAgentContext ctx;
  auto result = RunAsyncBlocking(rt, io, "parent", "child", "ping", ctx);

  ASSERT_TRUE(result.is_object());
  EXPECT_EQ(result.value("error", ""), "unknown_backend");
  EXPECT_EQ(result.value("backend", ""), "ghost");
  // The point of this test: the default backend must never have been used
  // as a silent fallback for the unregistered "ghost" name.
  EXPECT_EQ(default_backend->last_conversation(), nullptr);
}

// THE DECISIVE TEST for the tool_call_id defect the final re-review caught:
// SubAgentRuntime's own tool-dispatch loop (sub_agent_runtime.cc) built each
// tool result as {"name":..,"response":..} and never extracted the
// originating call's "id" — unlike AgentNode::Run's twin loop. On a
// REMOTE-shaped tool_calls entry (the OpenAI shape, carrying "id") this
// silently drops the id, so message_map.cc's ToOpenAiMessages rejects the
// resulting tool message ("...has no id...") on the sub-agent's second turn.
// This drives a real tool through the host registry and asserts the SENT
// second-turn message carries the originating id — not merely that the run
// avoided an error (dropping tools entirely would also avoid the error).
TEST(SubAgentRuntimeTest, ToolResultCarriesOriginatingCallId) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  host_tools.Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = "search",
                 .description = "test tool",
                 .params_json_schema = R"({"type":"object","properties":{}})"},
      [](std::string_view, const CancelToken&)
          -> asio::awaitable<std::string> { co_return "SEARCH_RESULT"; }));

  constexpr char kWf[] = R"({
    "schema_version":1,"name":"t","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{
      "parent":{"system_prompt":"","model":{},"tools":[],
                "delegates":{"agents":["child"],"max_depth":2}},
      "child":{"system_prompt":"","model":{},"tools":["search"]}
    },
    "main":"parent"
  })";
  auto wf = *WorkflowLoader::Load(kWf, host_tools);
  NullEventEmitter emit;

  // Turn 1: the model asks for a tool, REMOTE-shaped (carries "id", the
  // OpenAI tool_calls shape). Turn 2: it answers, so the loop reaches a
  // second turn and RunAsync must have echoed the id back on turn 1's
  // tool-result message.
  auto backend = std::make_shared<agentflow::testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[{"id":"call_7",)"
          R"("function":{"name":"search","arguments":"{\"q\":\"zen\"}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  SubAgentRuntime rt(wf, host_tools, emit,
                      SubAgentRuntime::DefaultConversationFactory(backend));
  SubAgentContext ctx;
  auto result = RunAsyncBlocking(rt, io, "parent", "child", "find zen", ctx);

  ASSERT_TRUE(result.is_string());
  EXPECT_EQ(result.get<std::string>(), "done");

  auto conv = backend->last_conversation();
  ASSERT_NE(conv, nullptr);
  ASSERT_EQ(conv->sent().size(), 2u);
  nlohmann::json tool_msg = nlohmann::json::parse(conv->sent()[1]);
  EXPECT_EQ(tool_msg["role"], "tool");
  ASSERT_EQ(tool_msg["content"].size(), 1u);
  EXPECT_EQ(tool_msg["content"][0]["id"], "call_7");
  EXPECT_EQ(tool_msg["content"][0]["name"], "search");
  EXPECT_EQ(tool_msg["content"][0]["response"]["value"], "SEARCH_RESULT");
}

}  // namespace
}  // namespace agentflow::workflow
