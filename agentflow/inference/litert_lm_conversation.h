// agentflow/inference/litert_lm_conversation.h
#ifndef AGENTFLOW_INFERENCE_LITERT_LM_CONVERSATION_H_
#define AGENTFLOW_INFERENCE_LITERT_LM_CONVERSATION_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <asio/awaitable.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/io_context.hpp>

#include "agentflow/inference/litert_lm_engine.h"
#include "c/engine.h"

namespace agentflow {

struct LiteRtLmConversationOptions {
  // JSON shape per LiteRT-LM C engine docs (engine.h §
  // litert_lm_conversation_config_create). All three are passed verbatim;
  // the engine handles templating.
  std::string system_message_json;  // e.g. {"role":"system","content":"..."}
  std::string tools_json = "[]";    // e.g. [{"type":"function",...}, ...]
  std::string messages_json = "[]"; // initial history
  bool enable_constrained_decoding = false;
  int max_output_tokens = 1024;
};

// Async streaming wrapper around a LiteRT-LM C conversation. The Conversation
// API (vs Session) feeds system+tools+messages separately to the engine,
// which applies the model's jinja chat template and returns parsed tool calls
// in a structured JSON response.
//
// Lifecycle: each instance owns one ::LiteRtLmConversation + its config.
// History is owned by the engine — calling SendMessage(m1) then SendMessage(m2)
// produces two turns. The caller does NOT need to thread message history
// between turns.
class LiteRtLmConversation {
 public:
  // Returns null on failure to construct the conversation config or object.
  static std::shared_ptr<LiteRtLmConversation> Create(
      std::shared_ptr<LiteRtLmEngine> engine,
      LiteRtLmConversationOptions opts,
      asio::io_context& io_ctx);

  ~LiteRtLmConversation();

  LiteRtLmConversation(const LiteRtLmConversation&) = delete;
  LiteRtLmConversation& operator=(const LiteRtLmConversation&) = delete;

  // Sends a user/tool-response message and starts streaming the model's reply.
  // Non-blocking. Subsequent NextTokenAsync() calls drain the chunks.
  // `extra_context` is opaque to us; pass "" unless the model docs say
  // otherwise.
  void SendMessage(std::string message_json, std::string extra_context = "");

  // Awaits the next streamed chunk. Empty string = end of this turn. Throws
  // std::runtime_error on stream error. After end-of-turn the caller may call
  // SendMessage again for the next turn.
  asio::awaitable<std::string> NextTokenAsync();

  // The full accumulated text (or JSON, depending on what the engine emits —
  // documented in P6 T2) of the most recent SendMessage turn. Populated as
  // chunks stream in; complete after NextTokenAsync returns the empty string.
  std::string FullResponseJson() const;

  // Cancels the in-flight request. Safe to call from any thread.
  void Cancel();

 private:
  LiteRtLmConversation(::LiteRtLmConversation* conv,
                       ::LiteRtLmConversationConfig* config,
                       std::shared_ptr<LiteRtLmEngine> engine,
                       asio::io_context& io_ctx);

  static void StreamCallback(void* data, const char* chunk,
                              bool is_final, const char* error_msg);

  std::shared_ptr<LiteRtLmEngine> engine_;  // keep engine alive
  ::LiteRtLmConversation* conv_;            // owned
  ::LiteRtLmConversationConfig* config_;    // owned
  asio::io_context& io_;
  asio::experimental::concurrent_channel<void(asio::error_code, std::string)> channel_;
  std::atomic<bool> cancelled_{false};

  mutable std::mutex accum_mu_;
  std::string accum_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_LITERT_LM_CONVERSATION_H_
