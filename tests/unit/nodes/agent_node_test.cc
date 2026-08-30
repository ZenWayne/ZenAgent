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
#include <asio/experimental/channel.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/chat_backend.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool_registry.h"
#include <algorithm>
#include <chrono>
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

// is_object() alone is not enough: json::value() still throws type_error.302
// on a PRESENT key with the wrong type, and type_error.306 when it is called
// on a non-object (e.g. tc["function"] being a bare string/number rather
// than an object). Every one of these entries used to abort the process;
// each must instead degrade to a skipped tool call.
TEST(AgentNodeTest, WrongTypedToolCallFieldsAreSkippedWithoutThrowing) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[)"
          R"({"name":42},)"
          R"({"name":null},)"
          R"({"function":"search"},)"
          R"({"function":7},)"
          R"({"id":12345},)"
          R"({"id":null},)"
          R"({"id":"call_1","function":{"name":"search","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = RegistryWith("search", "OK");
  EventCapture cap;
  State out = RunNode(std::move(cfg), "go", io, cap);

  // No malformed entry aborts the run; the one well-formed call still
  // dispatches and the run reaches its final answer.
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "done");

  auto conv = backend->last_conversation();
  ASSERT_NE(conv, nullptr);
  ASSERT_EQ(conv->sent().size(), 2u);
  json tool_msg = json::parse(conv->sent()[1]);
  EXPECT_EQ(tool_msg["role"], "tool");
  // 6 malformed entries dispatch with an empty name (skipped fields), plus
  // the one well-formed "call_1" entry — every entry still produces a
  // tool-result slot (a malformed entry degrades to an empty-name dispatch,
  // it never vanishes or crashes).
  ASSERT_EQ(tool_msg["content"].size(), 7u);
  EXPECT_EQ(tool_msg["content"][6]["name"], "search");
  EXPECT_EQ(tool_msg["content"][6]["id"], "call_1");
  EXPECT_EQ(tool_msg["content"][6]["response"]["value"], "OK");
}

TEST(AgentNodeTest, MultipleToolCallsInOneTurnRunConcurrently) {
  asio::io_context io;
  // Turn 1: two tool calls. Turn 2: final answer.
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[)"
          R"({"id":"a","function":{"name":"tool_a","arguments":"{}"}},)"
          R"({"id":"b","function":{"name":"tool_b","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  auto timeline = std::make_shared<std::vector<std::string>>();
  auto registry = std::make_shared<ToolRegistry>();
  auto add_tool = [&](std::string name, std::chrono::milliseconds delay) {
    registry->Register(std::make_shared<NativeFnTool>(
        ToolSchema{.name = name,
                   .description = "test tool",
                   .params_json_schema = R"({"type":"object","properties":{}})"},
        [name, delay, timeline](std::string_view, const CancelToken&)
            -> asio::awaitable<std::string> {
          timeline->push_back(name + "_start");
          if (delay.count() > 0) {
            auto exec = co_await asio::this_coro::executor;
            asio::steady_timer t(exec, delay);
            co_await t.async_wait(asio::use_awaitable);
          }
          timeline->push_back(name + "_end");
          co_return name + "_result";
        }));
  };
  add_tool("tool_a", std::chrono::milliseconds(50));
  add_tool("tool_b", std::chrono::milliseconds(0));

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = registry;
  EventCapture cap;
  State out = RunNode(std::move(cfg), "go", io, cap);
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "done");

  // Concurrency evidence: tool_b STARTS before tool_a FINISHES. A sequential
  // dispatch loop would produce a_start, a_end, b_start, b_end instead.
  ASSERT_EQ(timeline->size(), 4u);
  EXPECT_EQ((*timeline)[0], "tool_a_start");
  auto b_start = std::find(timeline->begin(), timeline->end(), "tool_b_start");
  auto a_end = std::find(timeline->begin(), timeline->end(), "tool_a_end");
  ASSERT_NE(b_start, timeline->end());
  ASSERT_NE(a_end, timeline->end());
  EXPECT_LT(std::distance(timeline->begin(), b_start),
            std::distance(timeline->begin(), a_end));
}

TEST(AgentNodeTest, ParallelResultsKeepOriginalCallOrderAndIds) {
  asio::io_context io;
  // Turn 1: three calls where call_1 is SLOW (completion order differs from
  // call order). Turn 2: final answer.
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[)"
          R"({"id":"call_1","function":{"name":"slow","arguments":"{}"}},)"
          R"({"id":"call_2","function":{"name":"fast","arguments":"{}"}},)"
          R"({"id":"call_3","function":{"name":"fast","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  auto registry = std::make_shared<ToolRegistry>();
  auto add_tool = [&](std::string name, std::string canned,
                      std::chrono::milliseconds delay) {
    registry->Register(std::make_shared<NativeFnTool>(
        ToolSchema{.name = name,
                   .description = "test tool",
                   .params_json_schema = R"({"type":"object","properties":{}})"},
        [canned = std::move(canned), delay](std::string_view, const CancelToken&)
            -> asio::awaitable<std::string> {
          if (delay.count() > 0) {
            auto exec = co_await asio::this_coro::executor;
            asio::steady_timer t(exec, delay);
            co_await t.async_wait(asio::use_awaitable);
          }
          co_return canned;
        }));
  };
  add_tool("slow", "R1", std::chrono::milliseconds(50));
  add_tool("fast", "R2", std::chrono::milliseconds(0));

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = registry;
  EventCapture cap;
  State out = RunNode(std::move(cfg), "go", io, cap);
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "done");

  // The tool-role message must preserve the ORIGINAL call order and echo each
  // originating id, regardless of completion order.
  auto conv = backend->last_conversation();
  ASSERT_NE(conv, nullptr);
  ASSERT_EQ(conv->sent().size(), 2u);
  json tool_msg = json::parse(conv->sent()[1]);
  EXPECT_EQ(tool_msg["role"], "tool");
  ASSERT_EQ(tool_msg["content"].size(), 3u);
  EXPECT_EQ(tool_msg["content"][0]["id"], "call_1");
  EXPECT_EQ(tool_msg["content"][0]["response"]["value"], "R1");
  EXPECT_EQ(tool_msg["content"][1]["id"], "call_2");
  EXPECT_EQ(tool_msg["content"][1]["response"]["value"], "R2");
  EXPECT_EQ(tool_msg["content"][2]["id"], "call_3");
  EXPECT_EQ(tool_msg["content"][2]["response"]["value"], "R2");
}

TEST(AgentNodeTest, EscapingToolExceptionYieldsErrorPlaceholderInPlace) {
  asio::io_context io;
  // Turn 1: good, BAD, good. The bad tool throws a NON-std::exception, which
  // escapes DispatchTool's `catch (const std::exception&)` and must surface as
  // the {"error":"tool_execution_failed"} placeholder in its own slot.
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[)"
          R"({"id":"call_1","function":{"name":"good","arguments":"{}"}},)"
          R"({"id":"call_2","function":{"name":"bad","arguments":"{}"}},)"
          R"({"id":"call_3","function":{"name":"good","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  auto registry = std::make_shared<ToolRegistry>();
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = "good",
                 .description = "test tool",
                 .params_json_schema = R"({"type":"object","properties":{}})"},
      [](std::string_view, const CancelToken&)
          -> asio::awaitable<std::string> { co_return std::string("OK"); }));
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = "bad",
                 .description = "test tool",
                 .params_json_schema = R"({"type":"object","properties":{}})"},
      [](std::string_view, const CancelToken&)
          -> asio::awaitable<std::string> { throw 42; }));

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = registry;
  EventCapture cap;
  State out = RunNode(std::move(cfg), "go", io, cap);
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "done");

  auto conv = backend->last_conversation();
  ASSERT_NE(conv, nullptr);
  json tool_msg = json::parse(conv->sent()[1]);
  ASSERT_EQ(tool_msg["content"].size(), 3u);
  EXPECT_EQ(tool_msg["content"][0]["response"]["value"], "OK");
  EXPECT_EQ(tool_msg["content"][1]["response"]["value"],
            R"({"error":"tool_execution_failed"})");
  EXPECT_EQ(tool_msg["content"][1]["id"], "call_2");  // slot keeps its id
  EXPECT_EQ(tool_msg["content"][2]["response"]["value"], "OK");
}

TEST(AgentNodeTest, CancellationPropagatesToSpawnedToolCalls) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[)"
          R"({"id":"a","function":{"name":"t","arguments":"{}"}},)"
          R"({"id":"b","function":{"name":"t","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  // Tool-side timeline: each invocation records "start", waits 20ms, then
  // records whether it observed the cancel. The run is cancelled 5ms in.
  // NOTE: deliberately does NOT assert on conv->sent() or the final answer —
  // AgentNode breaks its loop on cancel BEFORE sending the tool message back
  // (agent_node.cc: the per-iteration `if (cancel.IsCancelled()) break;` runs
  // before the next SendAsync), so no tool message ever reaches the fake
  // backend under EITHER implementation.
  auto timeline = std::make_shared<std::vector<std::string>>();
  auto registry = std::make_shared<ToolRegistry>();
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = "t",
                 .description = "test tool",
                 .params_json_schema = R"({"type":"object","properties":{}})"},
      [timeline](std::string_view, const CancelToken& cancel)
          -> asio::awaitable<std::string> {
        timeline->push_back("start");
        auto exec = co_await asio::this_coro::executor;
        asio::steady_timer t(exec, std::chrono::milliseconds(20));
        co_await t.async_wait(asio::use_awaitable);
        timeline->push_back(cancel.IsCancelled() ? "cancelled" : "done");
        co_return std::string("ok");
      }));

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = registry;
  auto node = std::make_unique<AgentNode>(std::move(cfg));
  test::TestState raw;
  raw.set_user_query("go");
  EventCapture cap;
  CancelSource cancel;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node->Run(State::From(std::move(raw)),
                                     cancel.Token(), cap.emitter);
      },
      asio::use_future);
  // Cancel 5ms in — mid tool execution, before the tools' 20ms timers fire.
  asio::steady_timer kill(io, std::chrono::milliseconds(5));
  kill.async_wait([&](asio::error_code) { cancel.Cancel(); });
  io.run();
  State out = fut.get();
  (void)out;

  // Both spawned coroutines observed the cancel AND both started before
  // either finished: [start, start, cancelled, cancelled]. A sequential
  // dispatch loop produces [start, cancelled, start, cancelled] — so this
  // asserts both cancel propagation AND concurrency, and is a genuine RED
  // test against the current sequential implementation.
  ASSERT_EQ(timeline->size(), 4u);
  EXPECT_EQ((*timeline)[0], "start");
  EXPECT_EQ((*timeline)[1], "start");
  EXPECT_EQ((*timeline)[2], "cancelled");
  EXPECT_EQ((*timeline)[3], "cancelled");
}

}  // namespace
}  // namespace agentflow
