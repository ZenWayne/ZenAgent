// tests/unit/nodes/agent_node_test.cc
//
// These cases used to require MODEL_PATH and were skipped in CI. AgentNode now
// takes an IChatBackend, so a scripted fake exercises the ReAct loop, tool
// dispatch and the iteration limit with no model file.

#include "agentflow/nodes/agent_node.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/chat_backend.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool_registry.h"
#include "test_messages.pb.h"
#include "tests/support/fake_chat_backend.h"

namespace agentflow {
namespace {

using json = nlohmann::json;

struct EventCapture {
  std::vector<proto::TraceEvent> events;
  std::mutex m;
  CallbackEventEmitter emitter{[this](const proto::TraceEvent& e) {
    std::lock_guard<std::mutex> l(m);
    events.push_back(e);
  }};

  std::vector<std::string> tokens() {
    std::lock_guard<std::mutex> l(m);
    std::vector<std::string> out;
    for (const auto& e : events) {
      if (e.has_token()) out.push_back(e.token().token());
    }
    return out;
  }
};

AgentNodeConfig BaseConfig(std::shared_ptr<IChatBackend> backend,
                            asio::io_context& io) {
  AgentNodeConfig cfg;
  cfg.backend = std::move(backend);
  cfg.io_ctx = &io;
  cfg.system_prompt = "You are a test agent.";
  cfg.input_field = "user_query";
  cfg.output_field = "assistant_reply";
  cfg.stream_tokens = true;
  return cfg;
}

// Runs the node to completion on `io` and returns the resulting State.
//
// AgentNode::Run() keeps its Conversation alive by registering
// cancel.OnCancel() on the CALLER's CancelSource (agent_node.cc's
// "Cooperative cancellation" hook), but that is no longer the only reference:
// FakeChatBackend holds its last conversation STRONGLY (see
// tests/support/fake_chat_backend.h), so a test can inspect it via
// FakeChatBackend::last_conversation() (see
// ToolCallIsDispatchedAndItsIdEchoedBack below) even after RunNode's local
// CancelSource is destroyed.
State RunNode(AgentNodeConfig cfg, const std::string& query,
               asio::io_context& io, EventCapture& cap) {
  auto node = std::make_unique<AgentNode>(std::move(cfg));
  test::TestState raw;
  raw.set_user_query(query);

  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        State state = State::From(std::move(raw));
        co_return co_await node->Run(std::move(state), cancel.Token(),
                                      cap.emitter);
      },
      asio::use_future);
  io.run();
  return fut.get();
}

std::shared_ptr<ToolRegistry> RegistryWith(
    const std::string& name, std::string canned_result) {
  auto registry = std::make_shared<ToolRegistry>();
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = name,
                 .description = "test tool",
                 .params_json_schema = R"({"type":"object","properties":{}})"},
      [canned = std::move(canned_result)](std::string_view,
                                           const CancelToken&)
          -> asio::awaitable<std::string> { co_return canned; }));
  return registry;
}

TEST(AgentNodeTest, PlainAnswerIsWrittenToOutputField) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","content":[{"type":"text","text":"42"}]})"});

  EventCapture cap;
  State out = RunNode(BaseConfig(backend, io), "what is 6*7?", io, cap);
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "42");
}

TEST(AgentNodeTest, SystemPromptAndToolsReachTheBackend) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","content":[{"type":"text","text":"ok"}]})"});

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = RegistryWith("search", "result");
  EventCapture cap;
  RunNode(std::move(cfg), "hi", io, cap);

  // The system message is a BARE content array, not a {role,content} object.
  EXPECT_EQ(json::parse(backend->last_options().system_message_json),
            json::parse(R"([{"type":"text","text":"You are a test agent."}])"));

  // BuildToolsJson already emits the OpenAI tools shape.
  json tools = json::parse(backend->last_options().tools_json);
  ASSERT_TRUE(tools.is_array());
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0]["type"], "function");
  EXPECT_EQ(tools[0]["function"]["name"], "search");
}

TEST(AgentNodeTest, ToolCallIsDispatchedAndItsIdEchoedBack) {
  asio::io_context io;
  // Turn 1: the model asks for a tool. Turn 2: it answers.
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[{"id":"call_7",)"
          R"("function":{"name":"search","arguments":"{\"q\":\"zen\"}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"found it"}]})"});

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = RegistryWith("search", "SEARCH_RESULT");
  EventCapture cap;
  State out = RunNode(std::move(cfg), "find zen", io, cap);

  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "found it");

  // The tool-result message must carry the originating call's id, so a remote
  // backend can restore OpenAI's tool_call_id (design spec §3.2).
  auto conv = backend->last_conversation();
  ASSERT_NE(conv, nullptr);
  ASSERT_EQ(conv->sent().size(), 2u);
  json tool_msg = json::parse(conv->sent()[1]);
  EXPECT_EQ(tool_msg["role"], "tool");
  ASSERT_EQ(tool_msg["content"].size(), 1u);
  EXPECT_EQ(tool_msg["content"][0]["id"], "call_7");
  EXPECT_EQ(tool_msg["content"][0]["name"], "search");
  EXPECT_EQ(tool_msg["content"][0]["response"]["value"], "SEARCH_RESULT");
}

TEST(AgentNodeTest, MaxIterReachedWritesTheFallbackMessage) {
  asio::io_context io;
  // Always asks for a tool, never answers.
  std::vector<std::string> loop(
      4,
      R"({"role":"assistant","tool_calls":[{"id":"c",)"
      R"("function":{"name":"noop","arguments":"{}"}}]})");
  auto backend = std::make_shared<testing::FakeChatBackend>(loop);

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = RegistryWith("noop", "");
  cfg.max_iter = 3;
  EventCapture cap;
  State out = RunNode(std::move(cfg), "spin", io, cap);

  EXPECT_EQ(out.As<test::TestState>().assistant_reply(),
            "Agent reached maximum iterations without a final answer.");
}

TEST(AgentNodeTest, StreamedDeltasReachTheEventEmitter) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","content":[{"type":"text","text":"ab"}]})"});
  backend->set_deltas({"a", "b"});

  EventCapture cap;
  RunNode(BaseConfig(backend, io), "hi", io, cap);
  EXPECT_EQ(cap.tokens(), (std::vector<std::string>{"a", "b"}));
}

TEST(AgentNodeTest, NonObjectToolCallEntryIsSkippedWithoutThrowing) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[42,{"id":"call_1",)"
          R"("function":{"name":"search","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = RegistryWith("search", "OK");
  EventCapture cap;
  State out = RunNode(std::move(cfg), "go", io, cap);

  // The bare 42 is skipped; the real call still dispatches and the run
  // completes instead of dying on an uncaught type_error.306.
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "done");
}

}  // namespace
}  // namespace agentflow
