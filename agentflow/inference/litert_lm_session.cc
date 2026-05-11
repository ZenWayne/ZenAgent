// agentflow/inference/litert_lm_session.cc
#include "agentflow/inference/litert_lm_session.h"

#include <stdexcept>
#include <system_error>

#include <asio/as_tuple.hpp>
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

  if (error_msg) {
    self->channel_.try_send(
        make_error_code(std::errc::io_error),
        std::string(error_msg));
    self->channel_.close();
    return;
  }

  self->channel_.try_send(
      asio::error_code{},
      chunk ? std::string(chunk) : std::string{});

  if (is_final) {
    self->channel_.close();
  }
}

}  // namespace agentflow
