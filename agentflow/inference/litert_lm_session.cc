// agentflow/inference/litert_lm_session.cc
#include "agentflow/inference/litert_lm_session.h"

#include <stdexcept>
#include <system_error>

#include <asio/as_tuple.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

namespace agentflow {

LiteRtLmSession::LiteRtLmSession(::LiteRtLmSession* session, asio::io_context& io)
    : session_(session),
      io_(io),
      channel_(io, 256) {}

LiteRtLmSession::~LiteRtLmSession() {
  Abort();
}

void LiteRtLmSession::Start(std::string input_text) {
  if (!session_) return;
  InputData input;
  input.type = kInputText;
  input.data = input_text.data();
  input.size = input_text.size();

  int rc = litert_lm_session_generate_content_stream(
      session_, &input, 1,
      &LiteRtLmSession::StreamCallback, this);
  if (rc != 0) {
    channel_.try_send(
        make_error_code(std::errc::io_error),
        "Failed to start LiteRT-LM stream");
    channel_.close();
  }
}

asio::awaitable<std::string> LiteRtLmSession::NextTokenAsync() {
  auto [ec, token] = co_await channel_.async_receive(
      asio::as_tuple(asio::use_awaitable));
  if (ec == asio::error::operation_aborted) {
    co_return std::string{};  // aborted
  }
  if (ec) {
    throw std::runtime_error("LiteRT-LM stream error: " + ec.message());
  }
  co_return token;
}

void LiteRtLmSession::Abort() {
  aborted_ = true;
  if (session_) {
    litert_lm_session_delete(session_);
    session_ = nullptr;
  }
  channel_.close();
}

void LiteRtLmSession::StreamCallback(void* data, const char* chunk,
                                      bool is_final, const char* error_msg) {
  auto* self = static_cast<LiteRtLmSession*>(data);
  if (self->aborted_) return;

  // LiteRT-LM invokes this from a background worker thread. asio channels are
  // not thread-safe, so marshal the touch onto the io_context. The C-string
  // args are only valid for this call, so copy before posting.
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
               self->channel_.try_send(asio::error_code{}, std::move(tok));
               if (is_final) self->channel_.close();
             });
}

}  // namespace agentflow
