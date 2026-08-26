// agentflow/inference/openai/openai_chat_backend.cc
#include "agentflow/inference/openai/openai_chat_backend.h"

#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include <asio/as_tuple.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

#include "agentflow/inference/openai/message_map.h"
#include "agentflow/inference/openai/stream_accumulator.h"

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

// Only transport-level and server-side failures are worth another attempt.
// A 4xx will fail identically every time.
bool IsRetryable(const absl::Status& s) {
  return s.code() == absl::StatusCode::kUnavailable ||
         s.code() == absl::StatusCode::kResourceExhausted;
}

class OpenAiConversation : public IConversation {
 public:
  OpenAiConversation(OpenAiOptions opts, net::IHttpClient& http,
                      ChatConversationOptions conv_opts)
      : opts_(std::move(opts)),
        http_(http),
        conv_opts_(std::move(conv_opts)) {
    if (auto sys = SystemMessage(conv_opts_.system_message_json)) {
      messages_.push_back(*std::move(sys));
    }
  }

  asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) override {
    auto incoming = ToOpenAiMessages(message_json);
    if (!incoming.ok()) co_return incoming.status();
    for (auto& m : *incoming) messages_.push_back(std::move(m));

    net::HttpRequest req;
    req.url = absl::StrCat(opts_.base_url, "/chat/completions");
    req.headers = {{"Content-Type", "application/json"}};
    // api_key is optional: some OpenAI-compatible endpoints (e.g. a local
    // Ollama) require no credential at all. Sending "Authorization: Bearer "
    // with nothing after it is not the same as sending no header, so omit it
    // entirely rather than encode an empty credential.
    if (!opts_.api_key.empty()) {
      req.headers.push_back(
          {"Authorization", absl::StrCat("Bearer ", opts_.api_key)});
    }
    req.body = BuildRequestBody(opts_.model, conv_opts_, messages_,
                                 /*stream=*/true);

    absl::Status last = absl::UnknownError("no attempt made");
    for (int attempt = 0; attempt < opts_.max_retries; ++attempt) {
      if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");

      StreamAccumulator acc;
      bool emitted = false;
      auto status = co_await http_.PostSse(
          req,
          [&](std::string_view frame) -> asio::awaitable<void> {
            std::string delta = acc.Feed(frame);
            if (delta.empty()) co_return;
            emitted = true;
            // co_await, so a slow consumer back-pressures the socket read.
            if (on_token) co_await on_token(delta);
            co_return;
          },
          cancel);

      if (status.ok()) {
        std::string canonical = acc.Canonical();
        // Record the assistant turn so the next Send resends full history —
        // HTTP is stateless, unlike the on-device engine.
        auto assistant = ToOpenAiMessages(canonical);
        if (assistant.ok()) {
          for (auto& m : *assistant) messages_.push_back(std::move(m));
        }
        co_return canonical;
      }

      last = status;
      if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");

      // The UI-protecting rule (design spec §6): once the user has seen part
      // of an answer, retrying would duplicate it on screen. Report instead.
      if (emitted) {
        co_return absl::Status(
            status.code(),
            absl::StrCat("stream interrupted after partial output: ",
                          status.message()));
      }
      if (!IsRetryable(status)) co_return status;
      if (attempt + 1 >= opts_.max_retries) break;

      // Exponential backoff: base, 2×base, 4×base…
      asio::steady_timer timer(co_await asio::this_coro::executor);
      timer.expires_after(opts_.retry_base_delay * (1 << attempt));
      auto [ec] = co_await timer.async_wait(
          asio::as_tuple(asio::use_awaitable));
      (void)ec;
    }
    co_return last;
  }

  void Cancel() override {
    // Deliberate no-op, NOT an unimplemented stub: this class owns no
    // transport handle to close. Every SendAsync call threads its
    // CancelToken straight through to http_.PostSse(), and HttpsClient
    // registers its own OnCancel hook on that same token (see
    // HttpsClient::Impl::PostSse in https_client.cc) that actually tears
    // down the live connection. Cancellation for a remote conversation is
    // real — it just lives in the HTTP client, not here. Do not remove the
    // HttpsClient registration believing this method is what owns
    // cancellation; if it is ever swapped out for a client that doesn't
    // hook the token, cancellation for this backend goes silently dead.
  }

 private:
  OpenAiOptions opts_;
  net::IHttpClient& http_;
  ChatConversationOptions conv_opts_;
  std::vector<json> messages_;
};

}  // namespace

OpenAiChatBackend::OpenAiChatBackend(OpenAiOptions opts, net::IHttpClient& http)
    : opts_(std::move(opts)),
      http_(http),
      describe_(absl::StrCat("openai:", opts_.model)) {}

std::shared_ptr<OpenAiChatBackend> OpenAiChatBackend::Create(
    OpenAiOptions opts, net::IHttpClient& http) {
  return std::shared_ptr<OpenAiChatBackend>(
      new OpenAiChatBackend(std::move(opts), http));
}

std::shared_ptr<IConversation> OpenAiChatBackend::CreateConversation(
    ChatConversationOptions opts) {
  if (opts.constrained_tool_calls) {
    // Do not degrade silently: an OpenAI-compatible endpoint has no equivalent
    // of LLGuidance grammar constraints, so the caller must be able to see
    // that the guarantee was dropped (design spec §6).
    last_warning_ = absl::StrCat(
        "backend ", describe_,
        " does not support constrained tool calls; running unconstrained");
  }
  return std::make_shared<OpenAiConversation>(opts_, http_, std::move(opts));
}

}  // namespace agentflow::openai
