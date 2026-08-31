// tests/integration/deep_search_e2e_test.cc
//
// Opt-in end-to-end check: the deep-search workflow against a real cloud
// endpoint with real Tavily traffic. Requires:
//   AGENTFLOW_LLM_BASE_URL, AGENTFLOW_LLM_MODEL, TAVILY_API_KEY
// Optional: AGENTFLOW_LLM_API_KEY, AGENTFLOW_LLM_CA_PATH
// Skipped by default so CI stays offline and free.
//
// Asserts STRUCTURE, never model prose:
//   - the run completes with a non-empty final answer,
//   - no delegate call returned an error placeholder,
//   - when the model issued >= 2 delegate calls, all delegate TOOL_CALL
//     events precede all delegate TOOL_RETURN events — the observable proof
//     they were spawned concurrently (a sequential dispatch loop would
//     interleave call/return pairs).
#include <algorithm>
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
#include "agentflow/core/state.h"
#include "agentflow/inference/openai/openai_chat_backend.h"
#include "agentflow/net/https_client.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "agentflow/workflow/workflow_runner.h"
#include "examples/deep-search/tavily_tools.h"
#include "test_messages.pb.h"

namespace agentflow {
namespace {

TEST(DeepSearchE2ETest, RealCloudRunFansOutAndGathers) {
  const char* base = std::getenv("AGENTFLOW_LLM_BASE_URL");
  const char* model = std::getenv("AGENTFLOW_LLM_MODEL");
  const char* tavily = std::getenv("TAVILY_API_KEY");
  if (!base || !model || !tavily) {
    GTEST_SKIP() << "AGENTFLOW_LLM_BASE_URL / AGENTFLOW_LLM_MODEL / "
                    "TAVILY_API_KEY not set";
  }
  const char* key = std::getenv("AGENTFLOW_LLM_API_KEY");   // optional
  const char* ca = std::getenv("AGENTFLOW_LLM_CA_PATH");    // optional

  asio::io_context io;
  net::HttpsClientOptions http_opts;
  http_opts.ca_path = ca ? ca : "/etc/ssl/certs/ca-certificates.crt";
  net::HttpsClient http(io, http_opts);

  openai::OpenAiOptions opts;
  opts.base_url = base;
  opts.model = model;
  if (key) opts.api_key = key;
  auto cloud = openai::OpenAiChatBackend::Create(opts, http);

  auto tools = std::make_shared<ToolRegistry>();
  tools->Register(deep_search::MakeTavilySearchTool(http, tavily));
  tools->Register(deep_search::MakeTavilyExtractTool(http, tavily));

  auto wf_or = workflow::WorkflowLoader::LoadFromFile(
      "examples/deep-search/workflow.json", *tools);
  ASSERT_TRUE(wf_or.ok()) << wf_or.status().message();

  workflow::AgentNodeBuildSpec spec;
  spec.workflow = *wf_or;
  spec.agent_name = "deep_search";
  spec.host_tools = tools;
  spec.io_ctx = &io;
  spec.backends["cloud"] = cloud;
  spec.max_iter = 12;

  auto built = workflow::BuildAgentNode(spec);
  AgentNode node(std::move(built.cfg));

  // Capture tool-call / tool-return events for the delegate tool.
  std::mutex mu;
  std::vector<proto::TraceEvent> events;
  CallbackEventEmitter emit([&](const proto::TraceEvent& e) {
    std::lock_guard<std::mutex> l(mu);
    events.push_back(e);
  });

  test::TestState state;
  state.set_user_query(
      "Compare the camera quality, battery life, and performance of the "
      "latest flagship smartphones from Apple, Samsung and Google, and "
      "recommend the best overall choice.");

  CancelSource cancel;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node.Run(State::From(std::move(state)),
                                    cancel.Token(), emit);
      },
      asio::use_future);
  io.run();
  State out = fut.get();

  // Structure assertion 1: a non-empty final answer was written.
  const std::string answer = out.As<test::TestState>().assistant_reply();
  EXPECT_FALSE(answer.empty()) << "final answer must be non-empty";

  // Collect delegate events in order.
  std::lock_guard<std::mutex> l(mu);
  std::vector<size_t> call_pos;
  std::vector<size_t> ret_pos;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].has_tool_call() &&
        events[i].tool_call().tool_name() == "delegate") {
      call_pos.push_back(i);
    }
    if (events[i].has_tool_return() &&
        events[i].tool_return().tool_name() == "delegate") {
      // Structure assertion 2: no error placeholder from any sub-agent.
      // Heuristic: this flags the literal "error" JSON key, so a legitimate
      // sub-agent report containing the word "error" would false-positive, and
      // a non-std exception escape emits no TOOL_RETURN at all (the count
      // assertion above catches that case).
      EXPECT_FALSE(events[i].tool_return().result_json().find("\"error\"") !=
                   std::string::npos)
          << "delegate returned an error: "
          << events[i].tool_return().result_json();
      ret_pos.push_back(i);
    }
  }

  // Structure assertion 3: if the model fanned out (>= 2 delegate calls),
  // every TOOL_CALL precedes every TOOL_RETURN — all were spawned before any
  // completed, i.e. concurrent dispatch. Sequential dispatch would
  // interleave call/return pairs.
  if (call_pos.size() >= 2) {
    ASSERT_EQ(call_pos.size(), ret_pos.size());
    size_t last_call = *std::max_element(call_pos.begin(), call_pos.end());
    size_t first_ret = *std::min_element(ret_pos.begin(), ret_pos.end());
    EXPECT_LT(last_call, first_ret)
        << "delegate calls were not dispatched concurrently";
  } else {
    GTEST_SKIP() << "model emitted fewer than 2 delegate calls; "
                    "concurrency assertion not applicable";
  }
}

}  // namespace
}  // namespace agentflow
