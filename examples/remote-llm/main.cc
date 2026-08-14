// examples/remote-llm/main.cc
//
// Runs one agent against an OpenAI-compatible endpoint. The workflow
// (workflow.json) names a logical backend ("cloud"); this host binary is the
// only place that resolves that name to a real base_url/api_key/model.
//
//   export AGENTFLOW_LLM_BASE_URL=https://api.deepseek.com/v1
//   export AGENTFLOW_LLM_MODEL=deepseek-chat
//   export AGENTFLOW_LLM_API_KEY=sk-...
//   bazel run //examples/remote-llm:remote_llm -- "your question"
//
// AGENTFLOW_LLM_API_KEY is OPTIONAL: some OpenAI-compatible endpoints (e.g.
// a local Ollama) need no credential at all. When set, it is sent as
// "Authorization: Bearer <key>"; when unset, the header is omitted entirely
// rather than sent empty.
//
// AGENTFLOW_LLM_CA_PATH is also optional: a CA bundle file or hashed CA
// directory used to verify an https:// base_url. Falls back to the desktop
// bundle (/etc/ssl/certs/ca-certificates.crt) when unset, so a local
// TLS-terminating test proxy with a self-signed cert can be verified without
// this file hardcoding its path.
//
// The key is read from the environment here. On Android the equivalent host
// code reads it from EncryptedSharedPreferences. Either way it is supplied by
// the HOST and never appears in the workflow JSON.
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/openai/openai_chat_backend.h"
#include "agentflow/net/https_client.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "agentflow/workflow/workflow_runner.h"
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

// AGENTFLOW_LLM_API_KEY and AGENTFLOW_LLM_CA_PATH are optional — see the
// file header. Empty means "not set".
std::string OptionalEnv(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string question =
      argc > 1 ? argv[1] : "Say hello in one short sentence.";

  asio::io_context io;

  // 1. Build the HTTP client and the remote backend. This is the ONLY place
  //    credentials appear.
  af::net::HttpsClientOptions http_opts;
  http_opts.ca_path = OptionalEnv("AGENTFLOW_LLM_CA_PATH");
  if (http_opts.ca_path.empty()) {
    http_opts.ca_path = "/etc/ssl/certs/ca-certificates.crt";
  }
  af::net::HttpsClient http(io, http_opts);

  af::openai::OpenAiOptions llm;
  llm.base_url = RequiredEnv("AGENTFLOW_LLM_BASE_URL");
  llm.model = RequiredEnv("AGENTFLOW_LLM_MODEL");
  llm.api_key = OptionalEnv("AGENTFLOW_LLM_API_KEY");
  auto cloud = af::openai::OpenAiChatBackend::Create(llm, http);

  // 2. Load the workflow and register the backend under its logical name.
  auto tools = std::make_shared<af::ToolRegistry>();
  auto wf_or = af::workflow::WorkflowLoader::LoadFromFile(
      "examples/remote-llm/workflow.json", *tools);
  if (!wf_or.ok()) {
    std::cerr << "failed to load workflow: " << wf_or.status().message()
              << "\n";
    return 1;
  }

  af::workflow::AgentNodeBuildSpec spec;
  spec.workflow = *wf_or;
  spec.agent_name = "assistant";
  spec.host_tools = tools;
  spec.io_ctx = &io;
  spec.backends["cloud"] = cloud;  // matches workflow.json's model.backend

  auto built = af::workflow::BuildAgentNode(spec);
  // BuildAgentNode only turns cfg.stream_tokens on when the caller wires a
  // run-wide TokenChannel via spec.token_channel (the JNI direct-push path).
  // We want the TraceEvent path instead — driven by the CallbackEventEmitter
  // below — so force streaming on explicitly.
  built.cfg.stream_tokens = true;
  af::AgentNode node(std::move(built.cfg));

  // 3. Stream the answer to stdout as it arrives.
  af::CallbackEventEmitter emit(
      [](const af::proto::TraceEvent& e) {
        if (e.has_token()) {
          std::cout << e.token().token() << std::flush;
        }
      });

  af::test::TestState state;  // replace with your own state proto
  state.set_user_query(question);

  af::CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<af::State> {
        co_return co_await node.Run(
            af::State::From(std::move(state)), cancel.Token(), emit);
      },
      asio::use_future);
  io.run();

  auto out = fut.get();
  std::cout << "\n---\n"
            << out.As<af::test::TestState>().assistant_reply() << "\n";
  return 0;
}
