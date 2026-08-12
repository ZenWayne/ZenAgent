// agentflow/inference/litert_lm_conversation.h
#ifndef AGENTFLOW_INFERENCE_LITERT_LM_CONVERSATION_H_
#define AGENTFLOW_INFERENCE_LITERT_LM_CONVERSATION_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/io_context.hpp>

#include "agentflow/inference/chat_backend.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "c/engine.h"

namespace agentflow {

// The LiteRT-specific name is retained as an alias so existing call sites
// and tests compile unchanged. The fields are identical; see
// agentflow/inference/chat_backend.h.
using LiteRtLmConversationOptions = ChatConversationOptions;

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

  // Sends a message synchronously and returns the model's full response JSON
  // (shape: {"role":"assistant","content":[{"type":"text","text":"..."}],
  // optionally "tool_calls":[...]}). Blocks until the engine is done.
  //
  // This is the preferred path. The streaming variant (SendMessage +
  // NextTokenAsync below) is broken in our current linkage — it fails inside
  // prompt_template_.Apply. The sync path goes through the same engine but
  // works because the rendering happens on the calling thread.
  absl::StatusOr<std::string> SendMessageSync(
      const std::string& message_json,
      const std::string& extra_context = "");

  // [Broken in current linkage — prefer SendMessageSync.] Sends a message and
  // starts streaming the model's reply. Subsequent NextTokenAsync() calls
  // drain the chunks.
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
  ::LiteRtLmConversationConfig* config_;    // owned; null when conv_ was
                                            // built via the constrained C
                                            // factory which doesn't take a
                                            // separate config handle.
  bool constrained_ = false;                // routes SendMessageSync to the
                                            // C bridge's constrained send.
  asio::io_context& io_;
  asio::experimental::concurrent_channel<void(asio::error_code, std::string)> channel_;
  std::atomic<bool> cancelled_{false};

  mutable std::mutex accum_mu_;
  std::string accum_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_LITERT_LM_CONVERSATION_H_
