// agentflow/inference/openai/openai_chat_backend.h
#ifndef AGENTFLOW_INFERENCE_OPENAI_OPENAI_CHAT_BACKEND_H_
#define AGENTFLOW_INFERENCE_OPENAI_OPENAI_CHAT_BACKEND_H_

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "agentflow/inference/chat_backend.h"
#include "agentflow/net/http_client.h"

namespace agentflow::openai {

struct OpenAiOptions {
  // Without a trailing slash; "/chat/completions" is appended.
  // e.g. "https://api.deepseek.com/v1", "http://127.0.0.1:11434/v1".
  std::string base_url;
  // Sent as "Authorization: Bearer <key>". NEVER placed in the request body,
  // in Describe(), or in any error message. Optional: some OpenAI-compatible
  // endpoints (e.g. a local Ollama) require no credential; when empty, the
  // Authorization header is omitted entirely rather than sent with an empty
  // credential.
  std::string api_key;
  std::string model;

  // Total attempts, not retries-after-the-first. 1 disables retrying.
  int max_retries = 3;
  std::chrono::milliseconds retry_base_delay{100};
};

// IChatBackend over an OpenAI-compatible /v1/chat/completions endpoint.
//
// Covers OpenAI, DeepSeek, Volcengine ARK, Kimi, GLM, MiniMax, OpenRouter,
// Ollama, vLLM and LiteLLM gateways — they differ only in base_url, api_key
// and model.
//
// HTTP is stateless, so each conversation owns its own messages array and
// resends the history every turn. (The on-device backend does the opposite:
// the engine owns history so the KV cache is reused. Both satisfy the same
// IConversation contract.)
class OpenAiChatBackend : public IChatBackend {
 public:
  // `http` must outlive the backend and every conversation it creates.
  static std::shared_ptr<OpenAiChatBackend> Create(OpenAiOptions opts,
                                                    net::IHttpClient& http);

  std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) override;

  // "openai:<model>". Never contains the key.
  std::string_view Describe() const override { return describe_; }

  // The most recent capability warning, e.g. that constrained_tool_calls was
  // requested but cannot be honoured. Empty when none. Hosts surface this;
  // the backend never silently drops a correctness guarantee.
  const std::string& last_warning() const { return last_warning_; }

 private:
  OpenAiChatBackend(OpenAiOptions opts, net::IHttpClient& http);

  OpenAiOptions opts_;
  net::IHttpClient& http_;
  std::string describe_;
  std::string last_warning_;
};

}  // namespace agentflow::openai
#endif  // AGENTFLOW_INFERENCE_OPENAI_OPENAI_CHAT_BACKEND_H_
