// tests/unit/inference/chat_backend_test.cc
#include "agentflow/inference/chat_backend.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "tests/support/fake_chat_backend.h"

namespace agentflow {
namespace {

TEST(ChatBackendTest, FakeBackendReturnsScriptedCanonicalResponse) {
  asio::io_context io;
  testing::FakeChatBackend backend({R"({"role":"assistant","content":[{"type":"text","text":"hi"}]})"});

  auto conv = backend.CreateConversation(ChatConversationOptions{});
  ASSERT_NE(conv, nullptr);

  std::string got;
  CancelSource cancel_src;
  const CancelToken cancel = cancel_src.Token();
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    auto r = co_await conv->SendAsync(R"({"role":"user","content":[]})",
                                       TokenSink{}, cancel);
    // EXPECT_ (not ASSERT_) here: ASSERT_* expands to a bare `return;`,
    // which is ill-formed inside a C++20 coroutine (must be `co_return`).
    EXPECT_TRUE(r.ok());
    if (r.ok()) got = *r;
  }, asio::detached);
  io.run();

  EXPECT_EQ(got,
            R"({"role":"assistant","content":[{"type":"text","text":"hi"}]})");
}

TEST(ChatBackendTest, FakeBackendDeliversTextDeltasToTokenSink) {
  asio::io_context io;
  testing::FakeChatBackend backend(
      {R"({"role":"assistant","content":[{"type":"text","text":"ab"}]})"});
  backend.set_deltas({"a", "b"});

  auto conv = backend.CreateConversation(ChatConversationOptions{});
  std::vector<std::string> seen;
  CancelSource cancel_src;
  const CancelToken cancel = cancel_src.Token();

  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    auto r = co_await conv->SendAsync(
        R"({"role":"user","content":[]})",
        // TokenSink returns an awaitable, so the sink is a coroutine lambda.
        [&](std::string_view d) -> asio::awaitable<void> {
          seen.emplace_back(d);
          co_return;
        },
        cancel);
    // EXPECT_ (not ASSERT_) here: ASSERT_* expands to a bare `return;`,
    // which is ill-formed inside a C++20 coroutine (must be `co_return`).
    EXPECT_TRUE(r.ok());
  }, asio::detached);
  io.run();

  EXPECT_EQ(seen, (std::vector<std::string>{"a", "b"}));
}

TEST(ChatBackendTest, DescribeCarriesNoCredentials) {
  testing::FakeChatBackend backend({});
  EXPECT_EQ(backend.Describe(), "fake");
}

}  // namespace
}  // namespace agentflow
