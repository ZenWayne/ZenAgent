#ifndef AGENTFLOW_WORKFLOW_WORKFLOW_RUNNER_H_
#define AGENTFLOW_WORKFLOW_WORKFLOW_RUNNER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentflow/core/event.h"
#include "agentflow/core/token_channel.h"
#include "agentflow/inference/chat_backend.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow.h"

namespace asio { class io_context; }

namespace agentflow::workflow {

// Spec for building one agent's AgentNodeConfig from a Workflow.
struct AgentNodeBuildSpec {
  std::shared_ptr<Workflow> workflow;
  std::string agent_name;                       // which agent in the spec
  std::shared_ptr<ToolRegistry> host_tools;     // shared with caller

  // Default inference backend, used by any agent whose ModelSpec.backend is
  // empty.
  std::shared_ptr<::agentflow::IChatBackend> backend;

  // Named backends the host registered. An agent selects one by logical name
  // via ModelSpec.backend. Credentials live in the host-constructed instance,
  // never in the spec.
  std::map<std::string, std::shared_ptr<::agentflow::IChatBackend>> backends;

  ::asio::io_context* io_ctx = nullptr;
  EventEmitter* emit = nullptr;                  // null → NullEventEmitter for sub-agent traces
  std::string input_field  = "user_query";
  std::string output_field = "assistant_reply";
  int max_iter = 8;

  // Optional run-wide token stream. When set, the main agent streams onto it
  // and each delegated sub-agent gets its own per-call channel that drains up
  // to this one. Null → no streaming (default).
  TokenChannel* token_channel = nullptr;
};

// Result: configured AgentNodeConfig + keepalive holders (SubAgentRuntime
// + delegate tool) whose lifetime must match the AgentNode's.
struct BuiltAgentNode {
  AgentNodeConfig cfg;
  // Opaque shared_ptr holders to keep SubAgentRuntime and delegate Tool
  // alive as long as the AgentNode lives.
  std::vector<std::shared_ptr<void>> keepalive;
};

// Build an AgentNodeConfig for the given agent. If the agent has a
// delegates block, also constructs a SubAgentRuntime + delegate tool and
// attaches it via cfg.extra_tools. Caller must keep BuiltAgentNode.keepalive
// alive as long as the resulting AgentNode is used.
BuiltAgentNode BuildAgentNode(const AgentNodeBuildSpec& spec);

}  // namespace agentflow::workflow
#endif
