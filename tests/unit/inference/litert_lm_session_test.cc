// tests/unit/inference/litert_lm_session_test.cc
#include "agentflow/inference/litert_lm_session.h"
#include "agentflow/inference/litert_lm_engine.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

namespace agentflow {
namespace {

// LiteRT-LM session test requires a real model. Marked manual.
TEST(LiteRtLmSessionTest, DISABLED_StartAndStream) {
  const char* model_path = std::getenv("MODEL_PATH");
  ASSERT_NE(model_path, nullptr) << "MODEL_PATH env var required";

  auto engine = LiteRtLmEngine::Create(
      LiteRtLmEngineOptions{.model_path = model_path});
  ASSERT_NE(engine, nullptr);

  auto* raw_session = litert_lm_engine_create_session(
      engine->Get(),
      /*session_config=*/nullptr);

  asio::io_context io;
  LiteRtLmSession session(raw_session, io);

  session.Start("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");

  std::string full_output;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<void> {
        while (true) {
          std::string token = co_await session.NextTokenAsync();
          if (token.empty()) break;
          full_output += token;
        }
      },
      asio::use_future);

  std::thread t([&] { io.run(); });
  t.join();
  fut.get();

  EXPECT_FALSE(full_output.empty());
  std::cout << "LLM output: " << full_output << std::endl;
}

TEST(LiteRtLmSessionTest, CancelAborts) {
  // Verify Abort() is idempotent and doesn't crash with null session.
  asio::io_context io;
  LiteRtLmSession session(nullptr, io);
  session.Abort();  // should not crash
  SUCCEED();
}

}  // namespace
}  // namespace agentflow
