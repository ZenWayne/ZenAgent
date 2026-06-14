#ifndef AGENTFLOW_WORKFLOW_EVAL_CONTEXT_H_
#define AGENTFLOW_WORKFLOW_EVAL_CONTEXT_H_

#include <string>

#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/state.h"

namespace agentflow::workflow {

// Bundle of values reachable by `{{path}}` templates at evaluation time.
// Members may be null when the context doesn't apply (e.g. `tool_args` is
// null outside a delegate-tool call boundary).
struct EvalContext {
  const State* state             = nullptr;  // current scope's state
  const State* parent_state      = nullptr;  // parent scope (delegate only)
  const nlohmann::ordered_json* tool_args = nullptr;  // LLM-supplied args
  std::string workflow_name;
  std::string workflow_version;
  absl::Time now = absl::Now();
};

}  // namespace agentflow::workflow

#endif  // AGENTFLOW_WORKFLOW_EVAL_CONTEXT_H_
