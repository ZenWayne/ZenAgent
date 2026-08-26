// agentflow/inference/litert_lm_chat_backend.cc
#include "agentflow/inference/litert_lm_chat_backend.h"

#include <atomic>
#include <exception>
#include <string>

#include "absl/status/status.h"

#include "agentflow/inference/canonical_message.h"
#include "agentflow/inference/litert_lm_conversation.h"

namespace agentflow {
namespace {

class LiteRtLmChatConversation : public IConversation {
 public:
  LiteRtLmChatConversation(std::shared_ptr<LiteRtLmConversation> conv,
                            bool constrained)
      : conv_(std::move(conv)), constrained_(constrained) {}

  asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) override {
    // Register the in-flight cancel hook once: a cancel must break the engine
    // request mid-decode, not merely at the next turn boundary.
    if (!cancel_registered_.exchange(true)) {
      auto conv = conv_;
      cancel.OnCancel([conv]() { conv->Cancel(); });
    }

    // Non-streaming path. The constrained C bridge has no streaming variant
    // (litert_lm_conversation_send_message_stream ignores the grammar), and a
    // missing sink means nobody wants deltas.
    if (constrained_ || !on_token) {
      co_return conv_->SendMessageSync(message_json);
    }

    conv_->SendMessage(std::move(message_json));
    LiteRtStreamAssembler assembler;
    for (;;) {
      std::string chunk;
      try {
        chunk = co_await conv_->NextTokenAsync();
      } catch (const std::exception&) {
        co_return absl::InternalError(
            "litert-lm: streaming send failed mid-decode");
      }
      if (chunk.empty()) break;  // end of turn

      const size_t before = assembler.text_deltas().size();
      assembler.Feed(chunk);
      // Forward only newly produced text deltas, never the raw envelope.
      // co_await, so a slow consumer back-pressures the decode loop.
      for (size_t i = before; i < assembler.text_deltas().size(); ++i) {
        co_await on_token(assembler.text_deltas()[i]);
      }
    }
    co_return assembler.Canonical();
  }

  void Cancel() override { conv_->Cancel(); }

 private:
  std::shared_ptr<LiteRtLmConversation> conv_;
  bool constrained_;
  std::atomic<bool> cancel_registered_{false};
};

}  // namespace

std::shared_ptr<IChatBackend> LiteRtLmChatBackend::Create(
    std::shared_ptr<LiteRtLmEngine> engine, asio::io_context& io) {
  return std::shared_ptr<IChatBackend>(
      new LiteRtLmChatBackend(std::move(engine), io));
}

std::shared_ptr<IConversation> LiteRtLmChatBackend::CreateConversation(
    ChatConversationOptions opts) {
  if (!engine_) return nullptr;
  const bool constrained = opts.constrained_tool_calls;
  auto conv = LiteRtLmConversation::Create(engine_, std::move(opts), io_);
  if (!conv) return nullptr;
  return std::make_shared<LiteRtLmChatConversation>(std::move(conv),
                                                     constrained);
}

}  // namespace agentflow
