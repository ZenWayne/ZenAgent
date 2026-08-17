// tests/unit/inference/litert_lm_chat_backend_test.cc
//
// End-to-end decode needs a real engine, so that case requires MODEL_PATH and
// skips itself. What IS asserted without a model is the seam contract:
// construction, Describe(), and the documented null-engine failure mode.
#include "agentflow/inference/litert_lm_chat_backend.h"

#include <cstdlib>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/inference/canonical_message.h"
#include "agentflow/inference/litert_lm_engine.h"

namespace agentflow {
namespace {

TEST(LiteRtLmChatBackendTest, DescribeIsStableAndCredentialFree) {
  asio::io_context io;
  auto backend = LiteRtLmChatBackend::Create(nullptr, io);
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->Describe(), "litert-lm");
}

TEST(LiteRtLmChatBackendTest, NullEngineYieldsNullConversation) {
  // A null engine cannot build a conversation. CreateConversation returns
  // nullptr rather than throwing; callers treat that as engine_error.
  asio::io_context io;
  auto backend = LiteRtLmChatBackend::Create(nullptr, io);
  EXPECT_EQ(backend->CreateConversation(ChatConversationOptions{}), nullptr);
}

TEST(LiteRtLmChatBackendTest, SendAsyncReturnsCanonicalAssistantJson) {
  const char* model_path = std::getenv("MODEL_PATH");
  if (!model_path) GTEST_SKIP() << "MODEL_PATH not set";

  asio::io_context io;
  auto engine = LiteRtLmEngine::Create(
      LiteRtLmEngineOptions{.model_path = model_path});
  ASSERT_NE(engine, nullptr);

  auto backend = LiteRtLmChatBackend::Create(engine, io);
  ChatConversationOptions opts;
  opts.system_message_json = R"([{"type":"text","text":"Answer briefly."}])";
  auto conv = backend->CreateConversation(std::move(opts));
  ASSERT_NE(conv, nullptr);

  std::string got;
  std::vector<std::string> deltas;
  CancelSource cancel_src;
  const CancelToken cancel = cancel_src.Token();
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    auto r = co_await conv->SendAsync(
        R"({"role":"user","content":[{"type":"text","text":"Say hi."}]})",
        [&](std::string_view d) -> asio::awaitable<void> {
          deltas.emplace_back(d);
          co_return;
        },
        cancel);
    // EXPECT_, not ASSERT_: ASSERT_* expands to a bare `return;`, which does
    // not compile inside a coroutine. The hard assertion is after io.run().
    EXPECT_TRUE(r.ok()) << r.status().message();
    if (r.ok()) got = *r;
  }, asio::detached);
  io.run();

  ASSERT_FALSE(got.empty()) << "SendAsync produced no canonical response";
  EXPECT_NE(got.find("\"role\":\"assistant\""), std::string::npos);
  std::string joined;
  for (const auto& d : deltas) joined += d;
  EXPECT_EQ(joined, ExtractAssistantText(got));
}

}  // namespace
}  // namespace agentflow
