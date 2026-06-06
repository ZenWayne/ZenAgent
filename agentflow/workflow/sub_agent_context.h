#ifndef AGENTFLOW_WORKFLOW_SUB_AGENT_CONTEXT_H_
#define AGENTFLOW_WORKFLOW_SUB_AGENT_CONTEXT_H_

#include <cstdint>
#include <string>

#include "agentflow/core/cancel.h"

namespace agentflow::workflow {

// Threaded through every delegate call. depth and root_invocation_id
// propagate down the call chain; root stays constant for the whole tree.
struct SubAgentContext {
  uint32_t depth = 0;
  std::string root_invocation_id;
  const CancelToken* parent_cancel = nullptr;
};

}  // namespace agentflow::workflow
#endif
