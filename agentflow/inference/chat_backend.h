// agentflow/inference/chat_backend.h
#ifndef AGENTFLOW_INFERENCE_CHAT_BACKEND_H_
#define AGENTFLOW_INFERENCE_CHAT_BACKEND_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"

namespace agentflow {

// One text delta from the model.
//
// Returns an awaitable and is ALWAYS co_awaited by its caller, so a consumer
// that is not keeping up applies back-pressure to the decode loop instead of
// having its tokens dropped. An empty (falsy) sink means "nobody wants
// deltas" — check it before calling.
using TokenSink =
    std::function<asio::awaitable<void>(std::string_view delta)>;

struct ChatConversationOptions {
  // A bare content ARRAY, not a {role,content} object:
  //   [{"type":"text","text":"You are ..."}]
  // LiteRT-LM wraps it into {role:system, content:<this>} itself; see
  // AgentNode::BuildSystemMessageJson. Empty means no system message.
  std::string system_message_json;

  // OpenAI tools shape, which is what AgentNode::BuildToolsJson already
  // emits: [{"type":"function","function":{name,description,parameters}}].
  std::string tools_json = "[]";

  // Initial history, canonical shape.
  std::string messages_json = "[]";

  int max_output_tokens = 1024;

  // LiteRT-only: attach an LLGuidance grammar derived from tools_json.
  // A remote backend cannot honour this; it emits a trace warning and runs
  // unconstrained rather than degrading silently (design spec §6).
  bool constrained_tool_calls = false;
};

// One multi-turn conversation. The implementation OWNS history: locally the
// engine does (so KV cache is reused across turns), remotely an internal
// messages array does. Callers never thread history between turns.
class IConversation {
 public:
  virtual ~IConversation() = default;

  // Sends one message JSON and awaits the full canonical assistant JSON:
  //   {"role":"assistant",
  //    "content":[{"type":"text","text":"..."}],
  //    "tool_calls":[{"id":"...","function":{"name":"...",
  //                                          "arguments":"{...}"}}]}
  // When `on_token` is set, each text delta is delivered as it arrives.
  // The return value is always canonical, whatever the backend.
  virtual asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) = 0;

  // Breaks the in-flight request. Safe to call from any thread.
  virtual void Cancel() = 0;
};

class IChatBackend {
 public:
  virtual ~IChatBackend() = default;

  virtual std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) = 0;

  // For traces and error messages, e.g. "litert-lm" or
  // "openai:deepseek-chat". MUST NOT contain credentials.
  virtual std::string_view Describe() const = 0;
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_CHAT_BACKEND_H_
