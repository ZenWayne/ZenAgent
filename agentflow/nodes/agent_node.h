// agentflow/nodes/agent_node.h
#ifndef AGENTFLOW_NODES_AGENT_NODE_H_
#define AGENTFLOW_NODES_AGENT_NODE_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/node.h"
#include "agentflow/core/state.h"
#include "agentflow/core/token_channel.h"
#include "agentflow/inference/chat_backend.h"
#include "agentflow/tools/tool.h"
#include "agentflow/tools/tool_registry.h"

namespace agentflow {

struct AgentNodeConfig {
  // The inference backend. LiteRtLmChatBackend for on-device, or a remote
  // backend such as OpenAiChatBackend — AgentNode cannot tell them apart.
  std::shared_ptr<IChatBackend> backend;
  asio::io_context* io_ctx = nullptr;  // for LiteRtLmSession creation
  std::string system_prompt;
  std::shared_ptr<ToolRegistry> tool_registry;
  int max_iter = 8;
  int max_output_tokens = 512;
  bool stream_tokens = true;

  // Protobuf reflection: field names on the state message.
  std::string input_field;       // read user query from this field
  std::string output_field;      // write assistant reply to this field
  std::string messages_field;    // reserved — engine owns history as of P6.

  // Tool names to advertise in the LLM request.
  // If empty, all registered tools are exported.
  std::vector<std::string> tool_names;

  // Tools added inline (not through the shared ToolRegistry). Used by the
  // workflow runner to inject the auto-generated `delegate` tool for an
  // agent with a delegates block, without mutating the host's registry.
  // Extras take precedence over registry tools when names collide.
  std::vector<std::shared_ptr<Tool>> extra_tools;

  // When true AND tool_registry is set, drive the LiteRT-LM Conversation
  // through the constrained C bridge (LiteRT-LM/c/engine.h §
  // litert_lm_engine_create_constrained_conversation). The model's tool-call
  // arguments are then forced by an LLGuidance Lark grammar derived from
  // the tools' parameter schemas to match the schema exactly.
  bool constrained_tool_calls = false;

  // Optional direct token stream. When set (and streaming is active — i.e.
  // stream_tokens && !constrained_tool_calls), each generated text delta is
  // also pushed onto this channel as it arrives, giving the top of the stack
  // (e.g. the JNI bridge → a Kotlin callback/Flow) a direct streaming path
  // that bypasses the proto::TraceEvent observability stream. Non-owning; the
  // channel must outlive the run and live on the same io_context as io_ctx.
  TokenChannel* token_channel = nullptr;
};

class AgentNode : public Node {
 public:
  explicit AgentNode(AgentNodeConfig cfg);
  ~AgentNode() override = default;

  std::string_view Id() const override { return id_; }
  std::string_view Kind() const override { return "agent"; }

  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;

 private:
  std::string BuildSystemMessageJson() const;
  std::string BuildToolsJson() const;
  std::string BuildUserMessageJson(const State& state) const;
  asio::awaitable<std::string> DispatchTool(
      const std::string& name, const std::string& args,
      const std::string& call_id,
      const CancelToken& cancel, EventEmitter& emit);
  void WriteOutput(State& state, const std::string& text) const;

  AgentNodeConfig cfg_;
  std::string id_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_NODES_AGENT_NODE_H_
