// agentflow/inference/litert_lm_chat_backend.h
#ifndef AGENTFLOW_INFERENCE_LITERT_LM_CHAT_BACKEND_H_
#define AGENTFLOW_INFERENCE_LITERT_LM_CHAT_BACKEND_H_

#include <memory>
#include <string_view>
#include <utility>

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
class LiteRtLmChatBackend : public IChatBackend {
 public:
  // `engine` may be null; CreateConversation then returns nullptr.
  // `io` must outlive the backend and every conversation it creates.
  static std::shared_ptr<IChatBackend> Create(
      std::shared_ptr<LiteRtLmEngine> engine, asio::io_context& io);

  std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) override;

  std::string_view Describe() const override { return "litert-lm"; }

 private:
  LiteRtLmChatBackend(std::shared_ptr<LiteRtLmEngine> engine,
                       asio::io_context& io)
      : engine_(std::move(engine)), io_(io) {}

  std::shared_ptr<LiteRtLmEngine> engine_;
  asio::io_context& io_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_LITERT_LM_CHAT_BACKEND_H_
