#ifndef AGENTFLOW_WORKFLOW_SUB_AGENT_CONTEXT_H_
#define AGENTFLOW_WORKFLOW_SUB_AGENT_CONTEXT_H_

#include <cstdint>
#include <string>

#include "agentflow/core/cancel.h"
#include "agentflow/core/token_channel.h"

namespace agentflow::workflow {

// Threaded through every delegate call. depth and root_invocation_id
// propagate down the call chain; root stays constant for the whole tree.
struct SubAgentContext {
  uint32_t depth = 0;
  std::string root_invocation_id;
  const CancelToken* parent_cancel = nullptr;

  // Optional per-invocation direct token stream. When set (and the child is
  // unconstrained), each generated text delta is pushed onto this channel as
  // it streams. The delegate tool creates one channel per call and drains it
  // up to the top-level stream — different sub-agents use different channels.
  TokenChannel* token_channel = nullptr;
};

}  // namespace agentflow::workflow
#endif
