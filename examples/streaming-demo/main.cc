// examples/streaming-demo/main.cc
// Real token streaming demo: AgentNode drives the engine's async stream and
// forwards each text delta as a TOKEN trace event, observed here via a
// CallbackEventEmitter (the same shape a JNI/Kotlin Flow or UI would consume).
//
// Usage: MODEL_PATH=/path/to/model ./streaming_demo
//
// Graph: agent --> sink. The agent answers via a real model, streaming tokens.
//
// NOTE: real streaming uses the unconstrained path — the constrained-decoding
// C bridge has no streaming variant. See examples/agent-demo for the
// constrained (non-streaming) variant.

#include <chrono>
#include <iostream>
#include <memory>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/graph.h"
#include "agentflow/core/runner.h"
#include "agentflow/core/state.h"
#include "agentflow/core/stub_node.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool_registry.h"
#include "test_messages.pb.h"
#include "trace_event.pb.h"

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
      af::LiteRtLmEngineOptions{.model_path = model_path});
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
  af::AgentNodeConfig agent_cfg;
  agent_cfg.engine = engine;
  agent_cfg.io_ctx = &io;
  agent_cfg.system_prompt =
      "You are a helpful assistant. When asked about time, use the get_time tool.";
  agent_cfg.tool_registry = registry;
  agent_cfg.input_field = "user_query";
  agent_cfg.output_field = "assistant_reply";
  agent_cfg.messages_field = "messages";
  agent_cfg.max_iter = 5;
  // Real per-token streaming on the unconstrained path.
  agent_cfg.stream_tokens = true;
  agent_cfg.constrained_tool_calls = false;

  // ── Graph ───────────────────────────────────────────────
  af::GraphBuilder b;
  b.AddNode(std::make_unique<af::AgentNode>(std::move(agent_cfg)))
   .AddNode(std::make_unique<af::StubNode>("sink", 0ms, nullptr, nullptr))
   .AddEdge("agent", "sink");
  auto graph = b.Build();

  // ── State ───────────────────────────────────────────────
  af::test::TestState init;
  init.set_user_query("Hello! What time is it?");
  init.set_counter(0);

  // ── Run with a streaming token observer ─────────────────
  // Each TOKEN event carries one text delta. This is the in-process analogue
  // of marshalling tokens onto a Kotlin Flow / UI stream.
  std::cout << "Assistant (streaming): " << std::flush;
  af::CallbackEventEmitter trace([](const af::proto::TraceEvent& ev) {
    if (ev.kind() == af::proto::TraceEvent::TOKEN) {
      std::cout << ev.token().token() << std::flush;
    }
  });
  af::Runner::Options ropts;
  ropts.trace = &trace;
  af::Runner runner(std::move(graph), ropts);

  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<af::State> {
        co_return co_await runner.Run(af::State::From(init));
      },
      asio::use_future);
  io.run();
  auto out = fut.get();

  const auto& ts = out.As<af::test::TestState>();
  std::cout << "\n\n=== Final ===\n";
  std::cout << "Assistant: " << ts.assistant_reply() << std::endl;

  return ts.assistant_reply().empty() ? 1 : 0;
}
