// agentflow/nodes/llm_node.h
#ifndef AGENTFLOW_NODES_LLM_NODE_H_
#define AGENTFLOW_NODES_LLM_NODE_H_

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
#include "c/engine.h"  // LiteRtLmSamplerParams

namespace agentflow {

// Configuration for a single-shot LLM call. The LlmNode runs ONE
// LiteRtLmSession and writes the accumulated reply into `output_field`.
//
// It MAY include tool schemas in the conversation prompt (so the model can
// emit function-calling JSON), but it never dispatches a tool — the raw
// reply (which may carry a "tool_calls" payload) is left for the next node
// to interpret (AgentNode for ReAct, or a separate tool-dispatching node).
struct LlmNodeConfig {
  std::string id;
  std::shared_ptr<LiteRtLmEngine> engine;
  asio::io_context* io_ctx = nullptr;

  std::string system_prompt;

  LiteRtLmSamplerParams sampler{};
  int max_output_tokens = 512;
  bool stream_tokens = true;

  // Same protobuf-field-name conventions AgentNode uses, so LlmNode and
  // AgentNode can read/write the same State shape interchangeably.
  std::string input_field;       // read user query from this string field
  std::string output_field;      // write assistant reply into this string field
  std::string messages_field;    // optional repeated msg field to append turn into

  // Optional: publish tool schemas in the conversation prompt so the model
  // can emit function-calling JSON. LlmNode does NOT execute the call — the
  // raw model reply is written to output_field for the next node to dispatch.
  // Leave tool_registry null for plain completion.
  std::shared_ptr<ToolRegistry> tool_registry;
  std::vector<std::string> tool_names;  // empty + non-null registry = expose all
};

class LlmNode : public Node {
 public:
  explicit LlmNode(LlmNodeConfig cfg);
  ~LlmNode() override = default;

  std::string_view Id() const override { return cfg_.id; }
  std::string_view Kind() const override { return "llm"; }

  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;

 private:
  std::string BuildConversationJson(const State& state) const;
  void WriteOutput(State& state, const std::string& text) const;

  LlmNodeConfig cfg_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_NODES_LLM_NODE_H_
