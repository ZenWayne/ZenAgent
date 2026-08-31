// agentflow/inference/litert_lm_chat_backend.h
#ifndef AGENTFLOW_INFERENCE_LITERT_LM_CHAT_BACKEND_H_
#define AGENTFLOW_INFERENCE_LITERT_LM_CHAT_BACKEND_H_

#include <memory>
#include <string_view>
#include <utility>

#include <asio/experimental/channel.hpp>
#include <asio/io_context.hpp>

#include "agentflow/inference/chat_backend.h"
#include "agentflow/inference/litert_lm_engine.h"

namespace agentflow {

// IChatBackend over the on-device LiteRT-LM engine.
//
// Owns the stream-envelope handling that used to be duplicated in
// AgentNode::Run and SubAgentRuntime::DefaultConversationFactory. History
// lives in the engine, so successive SendAsync calls on one conversation form
// a multi-turn exchange and the KV cache is reused across turns.
//
// The backend serializes LLM calls across ALL conversations it creates: one
// engine-wide slot (a capacity-1 channel) is acquired before each SendAsync
// and released after, so at most one conversation is mid-prefill/decode at
// any moment. The LiteRT-LM engine is not reliable with multiple live
// sessions (multi-session prefill input-buffer failures and a teardown
// double-free); serializing keeps the engine single-session while tool calls
// (HTTP) still overlap outside the lock.
class LiteRtLmChatBackend : public IChatBackend {
 public:
  // `engine` may be null; CreateConversation then returns nullptr.
  // `io` must outlive the backend and every conversation it creates.
  static std::shared_ptr<IChatBackend> Create(
      std::shared_ptr<LiteRtLmEngine> engine, asio::io_context& io);

  std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) override;

  std::string_view Describe() const override { return "litert-lm"; }

  // Capacity-1 slot shared by all conversations this backend creates;
  // LiteRtLmChatConversation acquires it around each SendAsync.
  using EngineSlot = asio::experimental::channel<void(asio::error_code, bool)>;

 private:
  LiteRtLmChatBackend(std::shared_ptr<LiteRtLmEngine> engine,
                       asio::io_context& io)
      : engine_(std::move(engine)),
        io_(io),
        engine_slot_(std::make_shared<EngineSlot>(io.get_executor(), 1)) {
    asio::error_code ec;
    engine_slot_->try_send(ec, true);  // one free slot
  }

  std::shared_ptr<LiteRtLmEngine> engine_;
  asio::io_context& io_;
  std::shared_ptr<EngineSlot> engine_slot_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_LITERT_LM_CHAT_BACKEND_H_
