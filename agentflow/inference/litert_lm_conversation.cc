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

  struct Enabler : LiteRtLmConversation {
    Enabler(::LiteRtLmConversation* c, ::LiteRtLmConversationConfig* cfg,
            std::shared_ptr<LiteRtLmEngine> e, asio::io_context& io)
        : LiteRtLmConversation(c, cfg, std::move(e), io) {}
  };

  // ── P8 constrained decoding path: LLGuidance + auto Lark grammar ─────────
  // The constrained C bridge owns its own config internally, so we pass
  // nullptr for the config_ field. The bridge requires tools to be useful
  // (no tools → empty grammar → behaves as if unconstrained).
  if (opts.constrained_tool_calls && tools != nullptr) {
    ::LiteRtLmConversation* conv =
        litert_lm_engine_create_constrained_conversation(
            engine->Get(), sys, tools);
    if (!conv) return nullptr;
    auto self = std::make_shared<Enabler>(conv, /*cfg=*/nullptr,
                                            std::move(engine), io_ctx);
    self->constrained_ = true;
    return self;
  }

  // ── Standard path ────────────────────────────────────────────────────────
  const char* msgs = opts.messages_json.empty() || opts.messages_json == "[]"
                         ? nullptr
                         : opts.messages_json.c_str();
  // Upstream (90f42140) replaced the one-shot config_create(engine, ...) with
  // a builder + setters API. The engine is no longer a config argument — it is
  // passed to litert_lm_conversation_create instead.
  ::LiteRtLmConversationConfig* cfg = litert_lm_conversation_config_create();
  if (!cfg) return nullptr;
  litert_lm_conversation_config_set_system_message(cfg, sys);
  litert_lm_conversation_config_set_tools(cfg, tools);
  litert_lm_conversation_config_set_messages(cfg, msgs);
  // enable_constrained_decoding stays off; constrained tool-call decoding is
  // done through the P8 bridge (litert_lm_engine_create_constrained_conversation).
  litert_lm_conversation_config_set_enable_constrained_decoding(cfg, false);

  ::LiteRtLmConversation* conv = litert_lm_conversation_create(
      engine->Get(), cfg);
  if (!conv) {
    litert_lm_conversation_config_delete(cfg);
    return nullptr;
  }
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
  // Constrained conversations go through the P8 bridge — it attaches the
  // pre-built Lark grammar as the decoding constraint for this turn.
  if (constrained_) {
    ::LiteRtLmJsonResponse* resp =
        litert_lm_conversation_send_message_constrained(
            conv_, message_json.c_str());
    if (!resp) {
      return absl::InternalError("constrained send_message returned null");
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
  ::LiteRtLmJsonResponse* resp = litert_lm_conversation_send_message(
      conv_, message_json.c_str(),
      extra_context.empty() ? nullptr : extra_context.c_str(),
      /*optional_args=*/nullptr);
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

  // Pass nullptr (not "") when empty — matches SendMessageSync. An empty
  // string parses to a discarded JSON in CreateOptionalArgs and corrupts
  // optional_args.extra_context, which then fails prompt_template_.Apply.
  int rc = litert_lm_conversation_send_message_stream(
      conv_,
      message_json.c_str(),
      extra_context.empty() ? nullptr : extra_context.c_str(),
      /*optional_args=*/nullptr,
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

void LiteRtLmConversation::StreamCallback(void* data,
                                          const LiteRtLmStreamChunk* chunk) {
  auto* self = static_cast<LiteRtLmConversation*>(data);
  if (self->cancelled_) return;

  // Called from a LiteRT-LM worker thread. asio channels and our accum buffer
  // are not thread-safe — marshal everything onto io_.
  const char* error_msg = litert_lm_stream_chunk_get_error(chunk);
  if (error_msg) {
    asio::post(self->io_, [self, msg = std::string(error_msg)]() mutable {
      self->channel_.try_send(make_error_code(std::errc::io_error),
                              std::move(msg));
      self->channel_.close();
    });
    return;
  }

  const char* text = litert_lm_stream_chunk_get_text(chunk);
  bool is_final = litert_lm_stream_chunk_is_final(chunk);
  asio::post(self->io_,
             [self, tok = text ? std::string(text) : std::string{},
              is_final]() mutable {
               if (!tok.empty()) {
                 {
                   std::lock_guard<std::mutex> lk(self->accum_mu_);
                   self->accum_.append(tok);
                 }
                 self->channel_.try_send(asio::error_code{}, std::move(tok));
               }
               // End-of-turn is signalled by an empty-string sentinel, NOT by
               // closing the channel — closing it would make the conversation
               // single-use and break the next turn's SendMessage. The channel
               // is only closed on Cancel()/destruction.
               if (is_final) {
                 self->channel_.try_send(asio::error_code{}, std::string{});
               }
             });
}

}  // namespace agentflow
