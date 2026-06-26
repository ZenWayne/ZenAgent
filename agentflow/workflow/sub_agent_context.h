#ifndef AGENTFLOW_WORKFLOW_SUB_AGENT_CONTEXT_H_
#define AGENTFLOW_WORKFLOW_SUB_AGENT_CONTEXT_H_

#include <cstdint>
#include <string>

#include "agentflow/core/cancel.h"
#include "agentflow/core/token_sink.h"

namespace agentflow::workflow {

// Threaded through every delegate call. depth and root_invocation_id
// propagate down the call chain; root stays constant for the whole tree.
struct SubAgentContext {
  uint32_t depth = 0;
  std::string root_invocation_id;
  const CancelToken* parent_cancel = nullptr;

  // Optional direct token sink. When set (and the child is unconstrained), each
  // generated text delta is handed to it as it streams. The delegate tool wires
  // this to the run-wide sink, so every sub-agent shares it (fan-in is inherent
  // to a callback).
  TokenSink token_sink;
};

}  // namespace agentflow::workflow
#endif
