// examples/agent-demo/main.cc
// P2 demo: AgentNode + ToolRegistry in a graph, driven by LiteRT-LM.
//
// Usage: MODEL_PATH=/path/to/model ./agent_demo
//
// Graph:
//   entry --> agent --> sink
// The agent uses a real model to answer the user's query.

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/graph.h"
#include "agentflow/core/runner.h"
#include "agentflow/core/state.h"
#include "agentflow/core/stub_node.h"
#include "agentflow/inference/litert_lm_chat_backend.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool_registry.h"
#include "test_messages.pb.h"

namespace af = agentflow;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
  const char* model_path = std::getenv("MODEL_PATH");
  if (!model_path) {
    std::cerr << "MODEL_PATH env var required\n";
    return 1;
  }

  // ── Engine ──────────────────────────────────────────────
  auto engine = af::LiteRtLmEngine::Create(
      af::LiteRtLmEngineOptions{.model_path = model_path,
                                .model_family = af::ModelFamily::kGemma});
  if (!engine) {
    std::cerr << "Failed to create LiteRT-LM engine\n";
    return 1;
  }

  // ── Tools ───────────────────────────────────────────────
  auto registry = std::make_shared<af::ToolRegistry>();
  registry->Register(std::make_shared<af::NativeFnTool>(
      af::ToolSchema{
          .name = "get_time",
          .description = "Get the current time",
          .params_json_schema = "{}",
      },
      [](std::string_view, const af::CancelToken&) -> asio::awaitable<std::string> {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        co_return std::ctime(&tt);
      }));

  // ── Agent ───────────────────────────────────────────────
  asio::io_context io;
  auto backend = af::LiteRtLmChatBackend::Create(engine, io);
  af::AgentNodeConfig agent_cfg;
  agent_cfg.backend = backend;
  agent_cfg.io_ctx = &io;
  agent_cfg.system_prompt =
      "You are a helpful assistant. When asked about time, use the get_time tool.";
  agent_cfg.tool_registry = registry;
  agent_cfg.input_field = "user_query";
  agent_cfg.output_field = "assistant_reply";
  agent_cfg.messages_field = "messages";
  agent_cfg.max_iter = 5;
  agent_cfg.stream_tokens = true;
  // P8 C-bridge: force tool-call output to match the get_time schema via
  // LLGuidance Lark grammar.
  agent_cfg.constrained_tool_calls = true;

  // ── Graph ───────────────────────────────────────────────
  af::GraphBuilder b;
  b.AddNode(std::make_unique<af::AgentNode>(std::move(agent_cfg)))
   .AddNode(std::make_unique<af::StubNode>("sink", 0ms, nullptr, nullptr))
   .AddEdge("agent", "sink");
  auto graph = b.Build();
  std::cout << "GRAPH:\n" << graph.ToDotString() << "---\n";

  // ── State ───────────────────────────────────────────────
  af::test::TestState init;
  init.set_user_query("Hello! What time is it?");
  init.set_counter(0);

  // ── Run ─────────────────────────────────────────────────
  af::Runner runner(std::move(graph), af::Runner::Options{});
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<af::State> {
        co_return co_await runner.Run(af::State::From(init));
      },
      asio::use_future);
  io.run();
  auto out = fut.get();

  const auto& ts = out.As<af::test::TestState>();
  std::cout << "\n=== Final ===\n";
  std::cout << "Assistant: " << ts.assistant_reply() << std::endl;

  return ts.assistant_reply().empty() ? 1 : 0;
}
