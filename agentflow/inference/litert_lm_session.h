// agentflow/inference/litert_lm_session.h
#ifndef AGENTFLOW_INFERENCE_LITERT_LM_SESSION_H_
#define AGENTFLOW_INFERENCE_LITERT_LM_SESSION_H_

#include <atomic>
#include <string>
#include <system_error>

#include <asio/awaitable.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/io_context.hpp>

#include "c/engine.h"

namespace agentflow {

// Async streaming wrapper around a LiteRT-LM C session.
// Bridges background-thread streaming callback → asio awaitable via channel.
class LiteRtLmSession {
 public:
  // Takes ownership of the C opaque session pointer `session`.
  LiteRtLmSession(::LiteRtLmSession* session, asio::io_context& io);
  ~LiteRtLmSession();

  LiteRtLmSession(const LiteRtLmSession&) = delete;
  LiteRtLmSession& operator=(const LiteRtLmSession&) = delete;

  // Start streaming generation. Non-blocking.
  // `input_text` is the full conversation JSON (system + user + tools).
  void Start(std::string input_text);

  // Await the next decoded token. Empty string = stream ended.
  // Throws std::runtime_error on stream error.
  asio::awaitable<std::string> NextTokenAsync();

  // Cancel the running session (thread-safe). Closes the channel.
  void Abort();

 private:
  static void StreamCallback(void* data, const char* chunk,
                              bool is_final, const char* error_msg);

  ::LiteRtLmSession* session_;  // owned; deleted in Abort()
  asio::io_context& io_;
  asio::experimental::concurrent_channel<void(asio::error_code, std::string)> channel_;
  std::atomic<bool> aborted_{false};
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_LITERT_LM_SESSION_H_
