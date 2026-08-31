// agentflow/inference/litert_lm_chat_backend.cc
#include "agentflow/inference/litert_lm_chat_backend.h"

#include <atomic>
#include <exception>
#include <string>

#include <asio/as_tuple.hpp>
#include <asio/use_awaitable.hpp>

#include "absl/status/status.h"

#include "agentflow/inference/canonical_message.h"
#include "agentflow/inference/litert_lm_conversation.h"

namespace agentflow {
namespace {

class LiteRtLmChatConversation : public IConversation {
 public:
  LiteRtLmChatConversation(
      std::shared_ptr<LiteRtLmConversation> conv, bool constrained,
      std::shared_ptr<LiteRtLmChatBackend::EngineSlot> engine_slot)
      : conv_(std::move(conv)),
        constrained_(constrained),
        engine_slot_(std::move(engine_slot)) {}

  asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) override {
    // Register the in-flight cancel hook once: a cancel must break the engine
    // request mid-decode, not merely at the next turn boundary.
    if (!cancel_registered_.exchange(true)) {
      auto conv = conv_;
      cancel.OnCancel([conv]() { conv->Cancel(); });
    }

    // Acquire the engine-wide slot so this conversation is the only one
    // mid-prefill/decode. SlotRelease returns it on every exit path.
    if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");
    auto [lock_ec, _] = co_await engine_slot_->async_receive(
        asio::as_tuple(asio::use_awaitable));
    if (lock_ec) co_return absl::CancelledError("cancelled");
    struct SlotRelease {
      LiteRtLmChatBackend::EngineSlot* slot;
      ~SlotRelease() {
        asio::error_code ec;
        slot->try_send(ec, true);
      }
    } release{engine_slot_.get()};

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
  std::shared_ptr<LiteRtLmChatBackend::EngineSlot> engine_slot_;
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
  return std::make_shared<LiteRtLmChatConversation>(
      std::move(conv), constrained, engine_slot_);
}

}  // namespace agentflow
