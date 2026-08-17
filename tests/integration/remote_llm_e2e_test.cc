// tests/integration/remote_llm_e2e_test.cc
//
// Opt-in end-to-end check against a real endpoint. Requires:
//   AGENTFLOW_LLM_BASE_URL, AGENTFLOW_LLM_MODEL
// Optional:
//   AGENTFLOW_LLM_API_KEY - sent as "Authorization: Bearer <key>" when set;
//                           omitted entirely when unset (e.g. a local Ollama
//                           needs no key at all).
//   AGENTFLOW_LLM_CA_PATH - CA bundle file or hashed CA directory used to
//                           verify an https:// base_url. Falls back to the
//                           desktop bundle (/etc/ssl/certs/ca-certificates.crt)
//                           when unset, so a local TLS-terminating test proxy
//                           with a self-signed cert can be verified without
//                           this file hardcoding its path.
// Skipped by default so CI stays offline and free.
//
// Does not assert on model output text — it is nondeterministic, and this
// model (gemma) emits early streaming frames with empty content and a
// separate "reasoning" field, so early deltas legitimately can be empty.
// What's asserted is structure: a non-empty stream of deltas, a well-formed
// canonical response, and that the deltas concatenate to the extracted text.
#include <cstdlib>
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/inference/canonical_message.h"
#include "agentflow/inference/openai/openai_chat_backend.h"
#include "agentflow/net/https_client.h"

namespace agentflow {
namespace {

TEST(RemoteLlmE2ETest, RealEndpointStreamsAnAnswer) {
  const char* base = std::getenv("AGENTFLOW_LLM_BASE_URL");
  const char* model = std::getenv("AGENTFLOW_LLM_MODEL");
  if (!base || !model) {
    GTEST_SKIP() << "AGENTFLOW_LLM_BASE_URL / AGENTFLOW_LLM_MODEL not set";
  }
  const char* key = std::getenv("AGENTFLOW_LLM_API_KEY");  // optional
  const char* ca = std::getenv("AGENTFLOW_LLM_CA_PATH");   // optional

  asio::io_context io;
  net::HttpsClientOptions http_opts;
  http_opts.ca_path = ca ? ca : "/etc/ssl/certs/ca-certificates.crt";
  net::HttpsClient http(io, http_opts);

  openai::OpenAiOptions opts;
  opts.base_url = base;
  opts.model = model;
  if (key) opts.api_key = key;
  auto backend = openai::OpenAiChatBackend::Create(opts, http);

  ChatConversationOptions conv_opts;
  conv_opts.system_message_json =
      R"([{"type":"text","text":"Answer in one short sentence."}])";
  auto conv = backend->CreateConversation(std::move(conv_opts));
  ASSERT_NE(conv, nullptr);

  std::string deltas;
  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await conv->SendAsync(
            R"({"role":"user","content":[{"type":"text","text":"Say hello."}]})",
            [&](std::string_view d) -> asio::awaitable<void> {
              deltas.append(d);
              co_return;
            },
            cancel.Token());
      },
      asio::use_future);
  io.run();

  auto resp = fut.get();
  ASSERT_TRUE(resp.ok()) << resp.status().message();
  EXPECT_FALSE(deltas.empty()) << "expected streamed deltas";
  EXPECT_EQ(deltas, ExtractAssistantText(*resp));
}

}  // namespace
}  // namespace agentflow
