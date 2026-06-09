#ifndef AGENTFLOW_WORKFLOW_DELEGATE_TOOL_H_
#define AGENTFLOW_WORKFLOW_DELEGATE_TOOL_H_

#include <memory>
#include <string>
#include <vector>

#include "agentflow/core/token_channel.h"
#include "agentflow/tools/tool.h"
#include "agentflow/workflow/sub_agent_context.h"

namespace asio { class io_context; }

namespace agentflow::workflow {

class SubAgentRuntime;

// Factory: built-in tool exposed to a parent agent with a delegates block.
// LLM calls delegate(agent="X", goal="..."). On invocation we spawn a fresh
// sub-agent via the bound SubAgentRuntime.
//
// When both `io` and `top_channel` are non-null, each delegate call gets its
// OWN TokenChannel (different sub-agents → different channels): the sub-agent
// streams deltas onto it and a drain coroutine forwards them up to
// `top_channel` (the run-wide stream the JNI/Kotlin layer consumes). Null →
// no sub-agent streaming.
std::shared_ptr<::agentflow::Tool> MakeDelegateTool(
    std::shared_ptr<SubAgentRuntime> runtime,
    std::string parent_agent,
    std::vector<std::string> allowed_children,
    SubAgentContext ctx,
    ::asio::io_context* io = nullptr,
    TokenChannel* top_channel = nullptr);

}  // namespace agentflow::workflow
#endif
