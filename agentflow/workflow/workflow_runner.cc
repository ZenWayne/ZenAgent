#include "agentflow/workflow/workflow_runner.h"

#include <utility>

#include <asio/io_context.hpp>

#include "agentflow/inference/litert_lm_chat_backend.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/workflow/delegate_tool.h"
#include "agentflow/workflow/sub_agent_context.h"
#include "agentflow/workflow/sub_agent_runtime.h"
#include "workflow_spec.pb.h"

namespace agentflow::workflow {

BuiltAgentNode BuildAgentNode(const AgentNodeBuildSpec& spec) {
  BuiltAgentNode out;
  AgentNodeConfig& cfg = out.cfg;
  // TODO(Task 6): AgentNodeBuildSpec.engine is replaced by .backend /
  // .backends there; this wraps the still-concrete engine in the seam just
  // enough to keep AgentNodeConfig.backend populated in the interim.
  if (spec.engine && spec.io_ctx) {
    cfg.backend = LiteRtLmChatBackend::Create(spec.engine, *spec.io_ctx);
  }
  cfg.io_ctx = spec.io_ctx;
  cfg.tool_registry = spec.host_tools;
  cfg.input_field = spec.input_field;
  cfg.output_field = spec.output_field;
  cfg.max_iter = spec.max_iter;
  // Stream the main agent when a run-wide channel is provided.
  cfg.token_channel = spec.token_channel;
  cfg.stream_tokens = spec.token_channel != nullptr;

  const auto& agents = spec.workflow->spec().agents();
  auto it = agents.find(spec.agent_name);
  if (it == agents.end()) return out;  // caller checks cfg validity
  const auto& agent_def = it->second;

  cfg.system_prompt = agent_def.system_prompt();
  cfg.constrained_tool_calls = agent_def.model().constrained_tool_calls();
  if (agent_def.model().max_output_tokens() > 0) {
    cfg.max_output_tokens = agent_def.model().max_output_tokens();
  }

  // Tool names declared by the agent (the LLM sees only these from the
  // registry).
  cfg.tool_names.clear();
  cfg.tool_names.reserve(agent_def.tools_size());
  for (const auto& t : agent_def.tools()) cfg.tool_names.push_back(t);

  // Auto-wire the delegate tool if this agent delegates.
  if (agent_def.has_delegates() && spec.engine && spec.io_ctx) {
    EventEmitter* emit = spec.emit;
    static NullEventEmitter kNullEmit;
    if (!emit) emit = &kNullEmit;

    auto runtime = std::make_shared<SubAgentRuntime>(
        spec.workflow, *spec.host_tools, *emit,
        SubAgentRuntime::DefaultConversationFactory(spec.engine, *spec.io_ctx));

    std::vector<std::string> allowed;
    allowed.reserve(agent_def.delegates().agents_size());
    for (const auto& a : agent_def.delegates().agents()) {
      allowed.push_back(a);
    }

    SubAgentContext sub_ctx;  // depth 0 at top level
    sub_ctx.root_invocation_id.clear();

    auto delegate = MakeDelegateTool(runtime, spec.agent_name,
                                       std::move(allowed), sub_ctx,
                                       spec.io_ctx, spec.token_channel);
    cfg.extra_tools.push_back(delegate);
    out.keepalive.push_back(runtime);
    out.keepalive.push_back(delegate);
  }
  return out;
}

}  // namespace agentflow::workflow
