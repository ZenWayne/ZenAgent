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

TEST(OpenAiChatBackendTest, ProviderControlsWhetherReasoningIsAdded) {
  for (const auto provider : {UsageProvider::kGoogle, UsageProvider::kDeepSeek, UsageProvider::kOpenAi}) {
    asio::io_context io;
    testing::FakeHttpClient http({{.frames = {
        R"({"choices":[],"usage":{"prompt_tokens":26,"completion_tokens":3,"completion_tokens_details":{"reasoning_tokens":84}}})"}}});
    auto options = TestOptions();
    // Identical model and URL for every provider: no model-name inference.
    options.provider = provider;
    auto backend = OpenAiChatBackend::Create(options, http);
    auto conv = backend->CreateConversation(ChatConversationOptions{});
    CancelSource cancel;
    auto result = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"hi"}]})", io, cancel.Token());
    ASSERT_TRUE(result.response.ok());
    EXPECT_EQ(json::parse(*result.response)["usage"]["output_tokens"], provider == UsageProvider::kGoogle ? 87 : 3);
  }
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

TEST(OpenAiChatBackendTest, EmptyApiKeyOmitsTheAuthorizationHeaderEntirely) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("ok")}}});
  auto opts = TestOptions();
  opts.api_key = "";               // keyless endpoint, e.g. a local Ollama
  auto backend = OpenAiChatBackend::Create(opts, http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  Send(*conv, R"({"role":"user","content":[{"type":"text","text":"hi"}]})", io,
       cancel.Token());

  ASSERT_EQ(http.requests().size(), 1u);
  for (const auto& [k, v] : http.requests()[0].headers) {
    EXPECT_NE(k, "Authorization")
        << "an empty key must send NO Authorization header, not an empty one";
  }
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

// --- Orphaned tool_calls in conversation history -------------------------
//
// Reproduces the "insufficient tool messages following tool_calls message"
// 400 seen against DeepSeek. A turn can end right after the model asked for
// tool calls but before their results are fed back -- the ReAct loop hits
// max_iter, the client aborts, or building the tool message throws. The
// assistant message carrying tool_calls is ALREADY in history at that point
// (SendAsync records it the moment the stream completes), so the NEXT user
// turn resends a history where an assistant tool_calls message is followed
// directly by a user message. OpenAI-compatible providers reject that:
//   400 An assistant message with 'tool_calls' must be followed by tool
//       messages responding to each 'tool_call_id'.
// The conversation must never emit such a history, however the previous turn
// ended.
std::string ToolCallFrame(const std::string& id, const std::string& name) {
  json f = {{"choices", json::array({{{"delta",
      {{"tool_calls", json::array({{{"index", 0}, {"id", id}, {"type", "function"},
        {"function", {{"name", name}, {"arguments", "{}"}}}}})}}}}})}};
  return f.dump();
}

TEST(OpenAiChatBackendTest, AbandonedToolCallTurnDoesNotPoisonNextRequest) {
  asio::io_context io;
  testing::FakeHttpClient http({
      {.frames = {ToolCallFrame("call_0", "get_project")}},  // turn 1: tool call
      {.frames = {TextFrame("done")}},                        // turn 2: next user msg
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});
  CancelSource cancel;

  // Turn 1: the model asks for a tool call. The caller then abandons the turn
  // (max_iter reached / user aborted) and never feeds a tool result back.
  auto r1 = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"a"}]})",
                 io, cancel.Token());
  ASSERT_TRUE(r1.response.ok());
  ASSERT_TRUE(json::parse(*r1.response).contains("tool_calls"));

  // Turn 2: a fresh user message on the SAME conversation.
  auto r2 = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"b"}]})",
                 io, cancel.Token());
  ASSERT_TRUE(r2.response.ok());

  // Every assistant tool_call in the sent history must be answered by a tool
  // message before any non-tool message follows it.
  ASSERT_EQ(http.requests().size(), 2u);
  json msgs = json::parse(http.requests()[1].body)["messages"];
  for (size_t i = 0; i < msgs.size(); ++i) {
    if (msgs[i].value("role", "") != "assistant" ||
        !msgs[i].contains("tool_calls")) {
      continue;
    }
    std::vector<std::string> want;
    for (const auto& tc : msgs[i]["tool_calls"]) {
      want.push_back(tc.value("id", ""));
    }
    std::vector<std::string> got;
    for (size_t j = i + 1; j < msgs.size(); ++j) {
      if (msgs[j].value("role", "") != "tool") break;
      got.push_back(msgs[j].value("tool_call_id", ""));
    }
    EXPECT_EQ(got, want)
        << "assistant message [" << i << "] has " << want.size()
        << " tool_calls but is followed by " << got.size()
        << " tool messages; provider rejects this with 400";
  }
}

constexpr char kImageMsg[] =
    R"({"role":"user","content":[{"type":"text","text":"看图"},{"type":"image_ref","key":"k1"}]})";

ChatConversationOptions WithResolver(int* calls, std::map<std::string, std::string> urls) {
  ChatConversationOptions o;
  o.image_ref_resolver = [calls, urls](std::vector<std::string> keys)
      -> asio::awaitable<absl::StatusOr<std::map<std::string, std::string>>> {
    ++*calls;
    std::map<std::string, std::string> out;
    for (const auto& k : keys) {
      if (auto it = urls.find(k); it != urls.end()) out[k] = it->second + "#" + std::to_string(*calls);
    }
    co_return out;
  };
  return o;
}

TEST(OpenAiChatBackendTest, ImageRefIsResolvedFreshOnEveryCall) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("a")}}, {.frames = {TextFrame("b")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  int calls = 0;
  auto conv = backend->CreateConversation(WithResolver(&calls, {{"k1", "https://cos/k1"}}));
  CancelSource cancel;

  ASSERT_TRUE(Send(*conv, kImageMsg, io, cancel.Token()).response.ok());
  ASSERT_TRUE(Send(*conv, R"({"role":"user","content":[{"type":"text","text":"再说说"}]})",
                   io, cancel.Token()).response.ok());

  ASSERT_EQ(http.requests().size(), 2u);
  json first = json::parse(http.requests()[0].body)["messages"].back()["content"][1];
  json second = json::parse(http.requests()[1].body)["messages"][0]["content"][1];
  EXPECT_EQ(first["image_url"]["url"], "https://cos/k1#1");
  EXPECT_EQ(second["image_url"]["url"], "https://cos/k1#2");  // 第二次调用重新签
  EXPECT_EQ(calls, 2);
}

TEST(OpenAiChatBackendTest, MissingImageFailsWithoutPoisoningHistory) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("ok")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  int calls = 0;
  auto conv = backend->CreateConversation(WithResolver(&calls, {}));  // k1 解析不到
  CancelSource cancel;

  auto r1 = Send(*conv, kImageMsg, io, cancel.Token());
  ASSERT_FALSE(r1.response.ok());
  EXPECT_EQ(r1.response.status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_NE(r1.response.status().message().find("attachment_invalid: k1"), std::string::npos);
  EXPECT_TRUE(http.requests().empty());  // 没有发给模型

  // 下一轮纯文本必须正常发出，且历史里没有那条失败的带图消息
  auto r2 = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"hi"}]})",
                 io, cancel.Token());
  ASSERT_TRUE(r2.response.ok());
  json msgs = json::parse(http.requests()[0].body)["messages"];
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0]["content"], "hi");
}

TEST(OpenAiChatBackendTest, ImageWithoutResolverIsRejected) {
  asio::io_context io;
  testing::FakeHttpClient http({});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});
  CancelSource cancel;
  auto r = Send(*conv, kImageMsg, io, cancel.Token());
  ASSERT_FALSE(r.response.ok());
  EXPECT_NE(r.response.status().message().find("attachment_invalid"), std::string::npos);
}


TEST(OpenAiChatBackendTest, ImageUrlsAreRefreshedForToolResultRound) {
  asio::io_context io;
  testing::FakeHttpClient http({
      {.frames = {ToolCallFrame("call_1", "inspect")}},
      {.frames = {TextFrame("done")}},
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  int calls = 0;
  auto conv = backend->CreateConversation(
      WithResolver(&calls, {{"k1", "https://cos/k1"}}));
  CancelSource cancel;
  ASSERT_TRUE(Send(*conv, kImageMsg, io, cancel.Token()).response.ok());
  ASSERT_TRUE(Send(*conv,
      R"({"role":"tool","content":[{"id":"call_1","name":"inspect","response":{"value":"ok"}}]})",
      io, cancel.Token()).response.ok());
  ASSERT_EQ(http.requests().size(), 2u);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(json::parse(http.requests()[1].body)["messages"][0]
                ["content"][1]["image_url"]["url"],
            "https://cos/k1#2");
}

TEST(OpenAiChatBackendTest, ImageUrlsAreRefreshedOnRetry) {
  asio::io_context io;
  testing::FakeHttpClient http({{.status = absl::UnavailableError("retry")}, {.frames = {TextFrame("ok")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  int calls = 0;
  auto conv = backend->CreateConversation(WithResolver(&calls, {{"k1", "https://cos/k1"}}));
  CancelSource cancel;
  ASSERT_TRUE(Send(*conv, kImageMsg, io, cancel.Token()).response.ok());
  ASSERT_EQ(http.requests().size(), 2u);
  EXPECT_EQ(json::parse(http.requests()[1].body)["messages"][0]["content"][1]["image_url"]["url"], "https://cos/k1#2");
}

TEST(OpenAiChatBackendTest, ExpiredHistoricalImageBecomesTextForFollowingTurn) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("ok")}}, {.frames = {TextFrame("next")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  int calls = 0;
  ChatConversationOptions options;
  options.image_ref_resolver = [&calls](std::vector<std::string>) -> asio::awaitable<absl::StatusOr<std::map<std::string, std::string>>> {
    if (++calls == 1) co_return std::map<std::string, std::string>{{"k1", "https://cos/k1"}};
    co_return std::map<std::string, std::string>{};
  };
  auto conv = backend->CreateConversation(options);
  CancelSource cancel;
  ASSERT_TRUE(Send(*conv, kImageMsg, io, cancel.Token()).response.ok());
  EXPECT_FALSE(Send(*conv, R"({"role":"user","content":"second"})", io, cancel.Token()).response.ok());
  ASSERT_TRUE(Send(*conv, R"({"role":"user","content":"third"})", io, cancel.Token()).response.ok());
  const auto messages = json::parse(http.requests().back().body)["messages"];
  ASSERT_EQ(messages.size(), 3u);  // first user, its answer, accepted third user
  EXPECT_EQ(messages[0]["content"][1]["text"], "[图片已失效]");
  EXPECT_EQ(messages.back()["content"], "third");
  for (const auto& message : messages) {
    EXPECT_NE(message["content"], "second")
        << "a rejected text turn must never be replayed to the model";
  }
  EXPECT_EQ(calls, 2);
}

TEST(OpenAiChatBackendTest, ResolverTransportFailureDoesNotRetainPlainTurn) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("first")}},
                                {.frames = {TextFrame("third")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  int calls = 0;
  ChatConversationOptions options;
  options.image_ref_resolver = [&calls](std::vector<std::string>)
      -> asio::awaitable<absl::StatusOr<std::map<std::string, std::string>>> {
    if (++calls == 2) co_return absl::UnavailableError("backend offline");
    co_return std::map<std::string, std::string>{{"k1", "https://cos/k1"}};
  };
  auto conv = backend->CreateConversation(options);
  CancelSource cancel;
  ASSERT_TRUE(Send(*conv, kImageMsg, io, cancel.Token()).response.ok());
  auto rejected = Send(*conv, R"({"role":"user","content":"second"})",
                       io, cancel.Token());
  EXPECT_EQ(rejected.response.status().code(), absl::StatusCode::kUnavailable);
  ASSERT_TRUE(Send(*conv, R"({"role":"user","content":"third"})",
                   io, cancel.Token()).response.ok());
  const auto messages = json::parse(http.requests().back().body)["messages"];
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages.back()["content"], "third");
  EXPECT_EQ(messages[0]["content"][1]["image_url"]["url"], "https://cos/k1");
  EXPECT_EQ(calls, 3);
}

TEST(OpenAiChatBackendTest, ResolverTransportFailureDoesNotRetainIncomingImage) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("ok")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  ChatConversationOptions options;
  options.image_ref_resolver = [](std::vector<std::string>) -> asio::awaitable<absl::StatusOr<std::map<std::string, std::string>>> {
    co_return absl::UnavailableError("backend offline");
  };
  auto conv = backend->CreateConversation(options);
  CancelSource cancel;
  auto first = Send(*conv, kImageMsg, io, cancel.Token());
  EXPECT_EQ(first.response.status().code(), absl::StatusCode::kUnavailable);
  ASSERT_TRUE(Send(*conv, R"({"role":"user","content":"retry text"})", io, cancel.Token()).response.ok());
  EXPECT_EQ(json::parse(http.requests()[0].body)["messages"].size(), 1u);
}

TEST(OpenAiChatBackendTest, SessionImageLimitRejectsTwentyFirstAndRollsBack) {
  asio::io_context io;
  std::vector<testing::FakeHttpTurn> turns(22);
  testing::FakeHttpClient http(turns);
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  int calls = 0;
  ChatConversationOptions options;
  options.image_ref_resolver = [&calls](std::vector<std::string> keys) -> asio::awaitable<absl::StatusOr<std::map<std::string, std::string>>> {
    ++calls;
    std::map<std::string, std::string> urls;
    for (const auto& key : keys) urls[key] = "https://cos/" + key;
    co_return urls;
  };
  auto conv = backend->CreateConversation(options);
  CancelSource cancel;
  for (int i = 0; i < 20; ++i) {
    auto message = json{{"role", "user"}, {"content", json::array({{{"type", "image_ref"}, {"key", std::to_string(i)}}})}}.dump();
    ASSERT_TRUE(Send(*conv, message, io, cancel.Token()).response.ok());
  }
  auto overflow = Send(*conv, kImageMsg, io, cancel.Token());
  EXPECT_FALSE(overflow.response.ok());
  EXPECT_EQ(calls, 20);
  ASSERT_TRUE(Send(*conv, R"({"role":"user","content":"still works"})", io, cancel.Token()).response.ok());
  EXPECT_EQ(http.requests().size(), 21u);
}
}  // namespace
}  // namespace agentflow::openai
