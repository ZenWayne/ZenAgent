#ifndef AGENTFLOW_WORKFLOW_DELEGATE_TOOL_H_
#define AGENTFLOW_WORKFLOW_DELEGATE_TOOL_H_

#include <memory>
#include <string>
#include <vector>

#include "agentflow/core/token_sink.h"
#include "agentflow/tools/tool.h"
#include "agentflow/workflow/sub_agent_context.h"

namespace agentflow::workflow {

class SubAgentRuntime;

// Factory: built-in tool exposed to a parent agent with a delegates block.
// LLM calls delegate(agent="X", goal="..."). On invocation we spawn a fresh
// sub-agent via the bound SubAgentRuntime.
//
// When `top_sink` is set, the spawned sub-agent streams each text delta to it
// (the run-wide sink the JNI/Kotlin layer consumes). Sub-agents share the one
// sink — fan-in is inherent to a callback, so there is no per-call channel or
// drain coroutine. Empty → no sub-agent streaming.
std::shared_ptr<::agentflow::Tool> MakeDelegateTool(
    std::shared_ptr<SubAgentRuntime> runtime,
    std::string parent_agent,
    std::vector<std::string> allowed_children,
    SubAgentContext ctx,
    TokenSink top_sink = {});

}  // namespace agentflow::workflow
#endif
