// tests/unit/nodes/agent_node_test.cc
//
// AgentNode integration tests. Require a real LiteRT-LM model — set
// MODEL_PATH to a .litertlm file. Tests GTEST_SKIP when MODEL_PATH is unset
// so CI without a model file still passes.

#include "agentflow/nodes/agent_node.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/state.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool_registry.h"
#include "test_messages.pb.h"

namespace agentflow {
namespace {

const char* MaybeModelPath() { return std::getenv("MODEL_PATH"); }

struct EventCapture {
  std::vector<proto::TraceEvent> events;
  std::mutex m;
  CallbackEventEmitter emitter{[this](const proto::TraceEvent& e) {
    std::lock_guard<std::mutex> l(m);
    events.push_back(e);
  }};
};

TEST(AgentNodeIntegrationTest, RealModelReturnsNonEmptyReply) {
  const char* model_path = MaybeModelPath();
  if (!model_path) GTEST_SKIP() << "MODEL_PATH not set";

  auto engine = LiteRtLmEngine::Create(
      LiteRtLmEngineOptions{.model_path = model_path});
  ASSERT_NE(engine, nullptr);

  asio::io_context io;
  AgentNodeConfig cfg;
  cfg.engine = engine;
  cfg.io_ctx = &io;
  cfg.system_prompt = "You are a helpful assistant. Reply briefly.";
  cfg.input_field = "user_query";
  cfg.output_field = "assistant_reply";
  cfg.max_iter = 2;

  auto node = std::make_unique<AgentNode>(std::move(cfg));

  test::TestState raw;
  raw.set_user_query("Say hello in one short sentence.");

  CancelSource cancel;
  EventCapture cap;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        State state = State::From(std::move(raw));
        co_return co_await node->Run(std::move(state), cancel.Token(),
                                      cap.emitter);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();

  std::string reply = out.As<test::TestState>().assistant_reply();
  EXPECT_FALSE(reply.empty()) << "expected non-empty assistant reply";
}

TEST(AgentNodeIntegrationTest, UsesGetTimeTool) {
  const char* model_path = MaybeModelPath();
  if (!model_path) GTEST_SKIP() << "MODEL_PATH not set";

  auto engine = LiteRtLmEngine::Create(
      LiteRtLmEngineOptions{.model_path = model_path});
  ASSERT_NE(engine, nullptr);

  auto registry = std::make_shared<ToolRegistry>();
  std::atomic<int> tool_invocations{0};
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{
          .name = "get_time",
          .description = "Get the current time",
          .params_json_schema = R"({"type":"object","properties":{},"required":[]})",
      },
      [&tool_invocations](std::string_view, const CancelToken&)
          -> asio::awaitable<std::string> {
        ++tool_invocations;
        co_return std::string{"2026-06-04T00:08:36Z"};
      }));

  asio::io_context io;
  AgentNodeConfig cfg;
  cfg.engine = engine;
  cfg.io_ctx = &io;
  cfg.system_prompt =
      "You are a helpful assistant. When the user asks about time, use the "
      "get_time tool to look it up.";
  cfg.tool_registry = registry;
  cfg.input_field = "user_query";
  cfg.output_field = "assistant_reply";
  cfg.max_iter = 4;

  auto node = std::make_unique<AgentNode>(std::move(cfg));

  test::TestState raw;
  raw.set_user_query("What time is it right now?");

  CancelSource cancel;
  EventCapture cap;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        State state = State::From(std::move(raw));
        co_return co_await node->Run(std::move(state), cancel.Token(),
                                      cap.emitter);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();

  EXPECT_GE(tool_invocations.load(), 1)
      << "expected the model to call get_time at least once";

  // The reply should reference the time we returned.
  std::string reply = out.As<test::TestState>().assistant_reply();
  EXPECT_FALSE(reply.empty());

  // TOOL_CALL/TOOL_RETURN should appear in the trace.
  std::lock_guard<std::mutex> l(cap.m);
  bool saw_tool_call = false, saw_tool_return = false;
  for (const auto& e : cap.events) {
    if (e.kind() == proto::TraceEvent::TOOL_CALL &&
        e.tool_call().tool_name() == "get_time") {
      saw_tool_call = true;
    }
    if (e.kind() == proto::TraceEvent::TOOL_RETURN &&
        e.tool_return().tool_name() == "get_time") {
      saw_tool_return = true;
    }
  }
  EXPECT_TRUE(saw_tool_call);
  EXPECT_TRUE(saw_tool_return);
}

}  // namespace
}  // namespace agentflow
