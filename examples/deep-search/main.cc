// examples/deep-search/main.cc
//
// Deep-search host. Backend chosen by environment:
//   - MODEL_PATH set       -> local LiteRT-LM engine (gemma-4-E2B-it.litertlm)
//   - MODEL_PATH unset     -> OpenAI-compatible cloud endpoint
// Both register under the logical name "cloud" that workflow.json uses, so
// the identical workflow runs unchanged in either mode.
//
//   export TAVILY_API_KEY=tvly-...
//   # cloud:
//   export AGENTFLOW_LLM_BASE_URL=https://api.deepseek.com/v1
//   export AGENTFLOW_LLM_MODEL=deepseek-chat
//   export AGENTFLOW_LLM_API_KEY=sk-...
//   bazel run //examples/deep-search:deep_search -- "your question"
//   # local (wall-time comparison):
//   MODEL_PATH=models/gemma-4-E2B-it.litertlm
//     bazel run //examples/deep-search:deep_search -- "your question"
//
// Credentials live only in the environment (host code) — never in
// workflow.json.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/litert_lm_chat_backend.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/inference/openai/openai_chat_backend.h"
#include "agentflow/net/https_client.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "agentflow/workflow/workflow_runner.h"
#include "examples/deep-search/tavily_tools.h"
#include "test_messages.pb.h"

namespace af = agentflow;

namespace {

std::string RequiredEnv(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    std::cerr << "missing required environment variable: " << name << "\n";
    std::exit(2);
  }
  return v;
}

std::string OptionalEnv(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string question =
      argc > 1 ? argv[1] : "What is the current state of on-device LLM "
                           "inference?";
  const std::string tavily_key = RequiredEnv("TAVILY_API_KEY");

  asio::io_context io;

  // HTTPS client is needed in BOTH modes (Tavily tools).
  af::net::HttpsClientOptions http_opts;
  http_opts.ca_path = OptionalEnv("AGENTFLOW_LLM_CA_PATH");
  if (http_opts.ca_path.empty()) {
    http_opts.ca_path = "/etc/ssl/certs/ca-certificates.crt";
  }
  af::net::HttpsClient http(io, http_opts);

  // Backend under the logical name "cloud" (what workflow.json names).
  std::shared_ptr<af::IChatBackend> cloud;
  std::string mode;
  const std::string model_path = OptionalEnv("MODEL_PATH");
  if (!model_path.empty()) {
    auto engine = af::LiteRtLmEngine::Create(
        af::LiteRtLmEngineOptions{.model_path = model_path,
                                  .model_family = af::ModelFamily::kGemma});
    if (!engine) {
      std::cerr << "failed to create LiteRT-LM engine\n";
      return 1;
    }
    cloud = af::LiteRtLmChatBackend::Create(engine, io);
    mode = "local:" + model_path;
  } else {
    af::openai::OpenAiOptions llm;
    llm.base_url = RequiredEnv("AGENTFLOW_LLM_BASE_URL");
    llm.model = RequiredEnv("AGENTFLOW_LLM_MODEL");
    llm.api_key = OptionalEnv("AGENTFLOW_LLM_API_KEY");
    cloud = af::openai::OpenAiChatBackend::Create(llm, http);
    mode = "cloud:" + llm.model;
  }

  // Tools: Tavily search + extract, registered host-side.
  auto tools = std::make_shared<af::ToolRegistry>();
  tools->Register(deep_search::MakeTavilySearchTool(http, tavily_key));
  tools->Register(deep_search::MakeTavilyExtractTool(http, tavily_key));

  auto wf_or = af::workflow::WorkflowLoader::LoadFromFile(
      "examples/deep-search/workflow.json", *tools);
  if (!wf_or.ok()) {
    std::cerr << "failed to load workflow: " << wf_or.status().message()
              << "\n";
    return 1;
  }

  af::workflow::AgentNodeBuildSpec spec;
  spec.workflow = *wf_or;
  spec.agent_name = "deep_search";
  spec.host_tools = tools;
  spec.io_ctx = &io;
  spec.backends["cloud"] = cloud;
  spec.max_iter = 12;

  auto built = af::workflow::BuildAgentNode(spec);
  built.cfg.stream_tokens = true;
  af::AgentNode node(std::move(built.cfg));

  af::CallbackEventEmitter emit(
      [](const af::proto::TraceEvent& e) {
        if (e.has_token()) std::cout << e.token().token() << std::flush;
      });

  af::test::TestState state;
  state.set_user_query(question);

  af::CancelSource cancel;
  auto start = std::chrono::steady_clock::now();
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<af::State> {
        co_return co_await node.Run(af::State::From(std::move(state)),
                                    cancel.Token(), emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  std::cout << "\n---\nmode=" << mode << "\nelapsed_ms=" << elapsed.count()
            << "\nanswer:\n"
            << out.As<af::test::TestState>().assistant_reply() << "\n";
  return 0;
}
