// tests/unit/core/cancel_test.cc
#include "agentflow/core/cancel.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>
#include <gtest/gtest.h>

namespace agentflow {
namespace {

TEST(CancelTest, FreshTokenNotCancelled) {
  CancelSource src;
  EXPECT_FALSE(src.Token().IsCancelled());
}

TEST(CancelTest, CancelFlipsToken) {
  CancelSource src;
  auto token = src.Token();
  EXPECT_FALSE(token.IsCancelled());
  src.Cancel();
  EXPECT_TRUE(token.IsCancelled());
}

TEST(CancelTest, OnCancelFiresOnce) {
  CancelSource src;
  std::atomic<int> fired{0};
  src.Token().OnCancel([&fired] { fired.fetch_add(1); });
  src.Cancel();
  EXPECT_EQ(fired.load(), 1);
  src.Cancel();   // idempotent
  EXPECT_EQ(fired.load(), 1);
}

TEST(CancelTest, OnCancelRegisteredAfterCancelFiresImmediately) {
  CancelSource src;
  src.Cancel();
  std::atomic<int> fired{0};
  src.Token().OnCancel([&fired] { fired.fetch_add(1); });
  EXPECT_EQ(fired.load(), 1);
}

TEST(CancelTest, WaitCancelledAwaitable) {
  asio::io_context io;
  CancelSource src;
  std::atomic<bool> resumed{false};

  asio::co_spawn(io,
    [&]() -> asio::awaitable<void> {
      co_await src.Token().WaitCancelled();
      resumed.store(true);
    },
    asio::detached);

  // Run io in a thread; trigger cancel from outside.
  std::thread t([&] { io.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(resumed.load());
  src.Cancel();
  t.join();
  EXPECT_TRUE(resumed.load());
}

}  // namespace
}  // namespace agentflow
