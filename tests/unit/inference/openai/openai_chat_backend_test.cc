// tests/unit/inference/openai/openai_chat_backend_test.cc
#include "agentflow/inference/openai/openai_chat_backend.h"

#include <memory>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "tests/support/fake_http_client.h"

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

OpenAiOptions TestOptions() {
  OpenAiOptions o;
  o.base_url = "https://api.example.com/v1";
  o.api_key = "sk-secret";
  o.model = "test-model";
  o.max_retries = 3;
  o.retry_base_delay = std::chrono::milliseconds(1);  // keep tests fast
  return o;
}

std::string TextFrame(const std::string& piece) {
  json f = {{"choices", json::array({{{"delta", {{"content", piece}}}}})}};
  return f.dump();
}

struct SendResult {
  absl::StatusOr<std::string> response;
  std::vector<std::string> deltas;
};

SendResult Send(IConversation& conv, const std::string& message_json,
                 asio::io_context& io, const CancelToken& cancel) {
  SendResult r;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await conv.SendAsync(
            message_json,
            [&](std::string_view d) -> asio::awaitable<void> {
              r.deltas.emplace_back(d);
              co_return;
            },
            cancel);
      },
      asio::use_future);
  io.run();
  io.restart();
  r.response = fut.get();
  return r;
}

TEST(OpenAiChatBackendTest, DescribeNamesTheModelAndHidesTheKey) {
  asio::io_context io;
  testing::FakeHttpClient http({});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  EXPECT_EQ(backend->Describe(), "openai:test-model");
  EXPECT_EQ(std::string(backend->Describe()).find("sk-secret"),
            std::string::npos);
}

TEST(OpenAiChatBackendTest, StreamsDeltasAndReturnsCanonicalJson) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("He"), TextFrame("llo")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"hi"}]})",
                 io, cancel.Token());

  ASSERT_TRUE(r.response.ok()) << r.response.status().message();
  EXPECT_EQ(r.deltas, (std::vector<std::string>{"He", "llo"}));
  EXPECT_EQ(json::parse(*r.response)["content"][0]["text"], "Hello");
}

TEST(OpenAiChatBackendTest, ApiKeyTravelsInTheHeaderNeverTheBody) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("ok")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  Send(*conv, R"({"role":"user","content":[{"type":"text","text":"hi"}]})", io,
       cancel.Token());

  ASSERT_EQ(http.requests().size(), 1u);
  const auto& req = http.requests()[0];
  EXPECT_EQ(req.url, "https://api.example.com/v1/chat/completions");
  EXPECT_EQ(req.body.find("sk-secret"), std::string::npos);
  bool found = false;
  for (const auto& [k, v] : req.headers) {
    if (k == "Authorization") {
      EXPECT_EQ(v, "Bearer sk-secret");
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(OpenAiChatBackendTest, HistoryIsOwnedSoTurnTwoCarriesTurnOne) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("first")}},
                                {.frames = {TextFrame("second")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  Send(*conv, R"({"role":"user","content":[{"type":"text","text":"one"}]})", io,
       cancel.Token());
  Send(*conv, R"({"role":"user","content":[{"type":"text","text":"two"}]})", io,
       cancel.Token());

  ASSERT_EQ(http.requests().size(), 2u);
  json body2 = json::parse(http.requests()[1].body);
  // user "one", assistant "first", user "two"
  ASSERT_EQ(body2["messages"].size(), 3u);
  EXPECT_EQ(body2["messages"][0]["content"], "one");
  EXPECT_EQ(body2["messages"][1]["role"], "assistant");
  EXPECT_EQ(body2["messages"][1]["content"], "first");
  EXPECT_EQ(body2["messages"][2]["content"], "two");
}

TEST(OpenAiChatBackendTest, RetriesUnavailableBeforeAnyTokenIsEmitted) {
  asio::io_context io;
  testing::FakeHttpClient http({
      {.status = absl::UnavailableError("503")},
      {.status = absl::UnavailableError("503")},
      {.frames = {TextFrame("ok")}},
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());

  ASSERT_TRUE(r.response.ok());
  EXPECT_EQ(http.attempts(), 3);
  EXPECT_EQ(r.deltas, (std::vector<std::string>{"ok"}));
}

TEST(OpenAiChatBackendTest, DoesNotRetryOnceATokenHasBeenEmitted) {
  // THE UI-protecting rule (design spec §6): retrying after the user has
  // already seen partial output would duplicate it on screen.
  asio::io_context io;
  testing::FakeHttpClient http({
      {.frames = {TextFrame("par")}, .status = absl::UnavailableError("dropped")},
      {.frames = {TextFrame("whole answer")}},  // must never be reached
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());

  EXPECT_FALSE(r.response.ok());
  EXPECT_EQ(http.attempts(), 1);
  EXPECT_EQ(r.deltas, (std::vector<std::string>{"par"}));
}

TEST(OpenAiChatBackendTest, DoesNotRetryClientErrors) {
  asio::io_context io;
  testing::FakeHttpClient http({
      {.status = absl::PermissionDeniedError("401 bad key")},
      {.frames = {TextFrame("never")}},
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());

  EXPECT_FALSE(r.response.ok());
  EXPECT_EQ(r.response.status().code(), absl::StatusCode::kPermissionDenied);
  EXPECT_EQ(http.attempts(), 1);
}

TEST(OpenAiChatBackendTest, GivesUpAfterMaxRetries) {
  asio::io_context io;
  testing::FakeHttpClient http({
      {.status = absl::UnavailableError("1")},
      {.status = absl::UnavailableError("2")},
      {.status = absl::UnavailableError("3")},
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());

  EXPECT_FALSE(r.response.ok());
  EXPECT_EQ(http.attempts(), 3);
}

TEST(OpenAiChatBackendTest, CancelDuringStreamingReturnsCancelledAndDoesNotRetry) {
  asio::io_context io;
  // Two frames scripted; the sink cancels on the first, so the second
  // must never be delivered.
  testing::FakeHttpClient http({{.frames = {TextFrame("a"), TextFrame("b")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  std::vector<std::string> deltas;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await conv->SendAsync(
            R"({"role":"user","content":[{"type":"text","text":"x"}]})",
            [&](std::string_view d) -> asio::awaitable<void> {
              deltas.emplace_back(d);
              cancel.Cancel();
              co_return;
            },
            cancel.Token());
      },
      asio::use_future);
  io.run();
  io.restart();

  auto resp = fut.get();
  EXPECT_FALSE(resp.ok());
  EXPECT_EQ(resp.status().code(), absl::StatusCode::kCancelled);
  EXPECT_EQ(deltas.size(), 1u);      // second frame never delivered
  EXPECT_EQ(http.attempts(), 1);     // and no retry after cancellation
}

TEST(OpenAiChatBackendTest, AlreadyCancelledTokenIssuesNoRequestAtAll) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("never")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  cancel.Cancel();
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());

  EXPECT_FALSE(r.response.ok());
  EXPECT_EQ(r.response.status().code(), absl::StatusCode::kCancelled);
  EXPECT_EQ(http.attempts(), 0);     // never hit the network at all
}

TEST(OpenAiChatBackendTest, ToolResultMessageBecomesOneOpenAiMessagePerResult) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("done")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  Send(*conv,
       R"({"role":"tool","content":[)"
       R"({"id":"c1","name":"a","response":{"value":"A"}},)"
       R"({"id":"c2","name":"b","response":{"value":"B"}}]})",
       io, cancel.Token());

  json body = json::parse(http.requests()[0].body);
  ASSERT_EQ(body["messages"].size(), 2u);
  EXPECT_EQ(body["messages"][0]["tool_call_id"], "c1");
  EXPECT_EQ(body["messages"][1]["tool_call_id"], "c2");
}

TEST(OpenAiChatBackendTest, ConstrainedToolCallsIsReportedNotSilentlyDropped) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("ok")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);

  ChatConversationOptions opts;
  opts.constrained_tool_calls = true;
  auto conv = backend->CreateConversation(std::move(opts));
  ASSERT_NE(conv, nullptr);  // still usable — it runs unconstrained

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());
  EXPECT_TRUE(r.response.ok());
  EXPECT_TRUE(backend->last_warning().find("constrained") != std::string::npos);
}

}  // namespace
}  // namespace agentflow::openai
