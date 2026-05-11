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
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/tools/tool_registry.h"

namespace agentflow {

struct AgentNodeConfig {
  std::shared_ptr<LiteRtLmEngine> engine;
  asio::io_context* io_ctx = nullptr;  // for LiteRtLmSession creation
  std::string system_prompt;
  std::shared_ptr<ToolRegistry> tool_registry;
  int max_iter = 8;
  int max_output_tokens = 512;
  bool stream_tokens = true;

  // Protobuf reflection: field names on the state message.
  std::string input_field;       // read user query from this field
  std::string output_field;      // write assistant reply to this field
  std::string messages_field;    // if non-empty, append to this repeated Message field

  // Tool names to advertise in the LLM request.
  // If empty, all registered tools are exported.
  std::vector<std::string> tool_names;
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
  std::string BuildConversationJson(const State& state) const;
  asio::awaitable<void> HandleToolCall(
      State& state, const std::string& call_id,
      const std::string& name,
      const std::string& args, const CancelToken& cancel);
  void WriteOutput(State& state, const std::string& text) const;

  AgentNodeConfig cfg_;
  std::string id_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_NODES_AGENT_NODE_H_
