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
#include <nlohmann/json.hpp>

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

// P8 C-bridge: with constrained_tool_calls=true, the assistant's tool call
// MUST contain every required key from the tool's parameter schema. We use a
// weather tool with two required keys (location + units). Without the
// constrained C bridge, small models frequently omit `units` on prompts that
// don't explicitly mention it; with LLGuidance Lark grammar from the tool
// schema, omission is impossible.
TEST(AgentNodeIntegrationTest, ConstrainedToolCallsHasAllRequiredKeys) {
  const char* model_path = MaybeModelPath();
  if (!model_path) GTEST_SKIP() << "MODEL_PATH not set";

  auto engine = LiteRtLmEngine::Create(
      LiteRtLmEngineOptions{.model_path = model_path});
  ASSERT_NE(engine, nullptr);

  auto registry = std::make_shared<ToolRegistry>();
  std::string captured_args;
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{
          .name = "weather",
          .description = "Get the weather for a location.",
          .params_json_schema = R"({"type":"object","properties":{)"
                                  R"("location":{"type":"string"},)"
                                  R"("units":{"type":"string","enum":["celsius","fahrenheit"]})"
                                  R"(},"required":["location","units"]})",
      },
      [&captured_args](std::string_view args,
                        const CancelToken&) -> asio::awaitable<std::string> {
        captured_args = std::string(args);
        co_return std::string{R"({"temp":72,"units":"fahrenheit"})"};
      }));

  asio::io_context io;
  AgentNodeConfig cfg;
  cfg.engine = engine;
  cfg.io_ctx = &io;
  cfg.system_prompt =
      "Use the weather tool. Always include both `location` and `units`.";
  cfg.tool_registry = registry;
  cfg.input_field = "user_query";
  cfg.output_field = "assistant_reply";
  cfg.max_iter = 3;
  cfg.constrained_tool_calls = true;  // <-- the thing under test

  auto node = std::make_unique<AgentNode>(std::move(cfg));
  test::TestState raw;
  raw.set_user_query("What is the weather in Paris?");

  CancelSource cancel;
  EventCapture cap;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node->Run(State::From(std::move(raw)),
                                      cancel.Token(), cap.emitter);
      },
      asio::use_future);
  io.run();
  (void)fut.get();

  ASSERT_FALSE(captured_args.empty()) << "tool was not invoked";
  nlohmann::json args = nlohmann::json::parse(captured_args);
  EXPECT_TRUE(args.contains("location"))
      << "constrained tool_call missing 'location'; got: " << captured_args;
  EXPECT_TRUE(args.contains("units"))
      << "constrained tool_call missing 'units'; got: " << captured_args;
}

}  // namespace
}  // namespace agentflow
