// agentflow/inference/litert_lm_conversation.cc
#include "agentflow/inference/litert_lm_conversation.h"

#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

namespace agentflow {

LiteRtLmConversation::LiteRtLmConversation(
    ::LiteRtLmConversation* conv,
    ::LiteRtLmConversationConfig* config,
    std::shared_ptr<LiteRtLmEngine> engine,
    asio::io_context& io_ctx)
    : engine_(std::move(engine)),
      conv_(conv),
      config_(config),
      io_(io_ctx),
      channel_(io_ctx, 256) {}

std::shared_ptr<LiteRtLmConversation> LiteRtLmConversation::Create(
    std::shared_ptr<LiteRtLmEngine> engine,
    LiteRtLmConversationOptions opts,
    asio::io_context& io_ctx) {
  if (!engine) return nullptr;

  // Pass NULL (not "") so the engine doesn't construct an empty preface
  // entry — litert_lm_main works by NOT calling SetPreface at all, and
  // the jinja template rendering differs when a preface is present even
  // with empty content.
  const char* sys = opts.system_message_json.empty()
                        ? nullptr
                        : opts.system_message_json.c_str();
  const char* tools = opts.tools_json.empty() || opts.tools_json == "[]"
                          ? nullptr
                          : opts.tools_json.c_str();
  const char* msgs = opts.messages_json.empty() || opts.messages_json == "[]"
                         ? nullptr
                         : opts.messages_json.c_str();
  ::LiteRtLmConversationConfig* cfg = litert_lm_conversation_config_create(
      engine->Get(),
      /*session_config=*/nullptr,
      sys, tools, msgs,
      opts.enable_constrained_decoding);
  if (!cfg) return nullptr;

  ::LiteRtLmConversation* conv = litert_lm_conversation_create(
      engine->Get(), cfg);
  if (!conv) {
    litert_lm_conversation_config_delete(cfg);
    return nullptr;
  }

  // shared_ptr aliased to private ctor via make_shared trick.
  struct Enabler : LiteRtLmConversation {
    Enabler(::LiteRtLmConversation* c, ::LiteRtLmConversationConfig* cfg,
            std::shared_ptr<LiteRtLmEngine> e, asio::io_context& io)
        : LiteRtLmConversation(c, cfg, std::move(e), io) {}
  };
  return std::make_shared<Enabler>(conv, cfg, std::move(engine), io_ctx);
}

LiteRtLmConversation::~LiteRtLmConversation() {
  Cancel();
  if (conv_) {
    litert_lm_conversation_delete(conv_);
    conv_ = nullptr;
  }
  if (config_) {
    litert_lm_conversation_config_delete(config_);
    config_ = nullptr;
  }
}

absl::StatusOr<std::string> LiteRtLmConversation::SendMessageSync(
    const std::string& message_json, const std::string& extra_context) {
  if (!conv_) {
    return absl::FailedPreconditionError("conversation not created");
  }
  ::LiteRtLmJsonResponse* resp = litert_lm_conversation_send_message(
      conv_, message_json.c_str(),
      extra_context.empty() ? nullptr : extra_context.c_str());
  if (!resp) {
    return absl::InternalError("send_message returned null");
  }
  const char* s = litert_lm_json_response_get_string(resp);
  std::string out = s ? std::string(s) : std::string{};
  litert_lm_json_response_delete(resp);
  {
    std::lock_guard<std::mutex> lk(accum_mu_);
    accum_ = out;
  }
  return out;
}

void LiteRtLmConversation::SendMessage(std::string message_json,
                                        std::string extra_context) {
  if (!conv_ || cancelled_) return;

  {
    std::lock_guard<std::mutex> lk(accum_mu_);
    accum_.clear();
  }

  int rc = litert_lm_conversation_send_message_stream(
      conv_,
      message_json.c_str(),
      extra_context.c_str(),
      &LiteRtLmConversation::StreamCallback,
      this);
  if (rc != 0) {
    channel_.try_send(
        make_error_code(std::errc::io_error),
        "Failed to start LiteRT-LM conversation stream");
    channel_.close();
  }
}

asio::awaitable<std::string> LiteRtLmConversation::NextTokenAsync() {
  auto [ec, token] = co_await channel_.async_receive(
      asio::as_tuple(asio::use_awaitable));
  if (ec == asio::error::operation_aborted) {
    co_return std::string{};
  }
  if (ec) {
    throw std::runtime_error(
        "LiteRT-LM conversation stream error: " + ec.message());
  }
  co_return token;
}

std::string LiteRtLmConversation::FullResponseJson() const {
  std::lock_guard<std::mutex> lk(accum_mu_);
  return accum_;
}

void LiteRtLmConversation::Cancel() {
  cancelled_ = true;
  if (conv_) {
    litert_lm_conversation_cancel_process(conv_);
  }
  channel_.close();
}

void LiteRtLmConversation::StreamCallback(void* data, const char* chunk,
                                           bool is_final,
                                           const char* error_msg) {
  auto* self = static_cast<LiteRtLmConversation*>(data);
  if (self->cancelled_) return;

  // Called from a LiteRT-LM worker thread. asio channels and our accum buffer
  // are not thread-safe — marshal everything onto io_.
  if (error_msg) {
    asio::post(self->io_, [self, msg = std::string(error_msg)]() mutable {
      self->channel_.try_send(make_error_code(std::errc::io_error),
                              std::move(msg));
      self->channel_.close();
    });
    return;
  }

  asio::post(self->io_,
             [self, tok = chunk ? std::string(chunk) : std::string{},
              is_final]() mutable {
               if (!tok.empty()) {
                 std::lock_guard<std::mutex> lk(self->accum_mu_);
                 self->accum_.append(tok);
               }
               self->channel_.try_send(asio::error_code{}, std::move(tok));
               if (is_final) self->channel_.close();
             });
}

}  // namespace agentflow
