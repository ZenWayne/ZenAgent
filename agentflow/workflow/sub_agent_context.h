#ifndef AGENTFLOW_WORKFLOW_SUB_AGENT_CONTEXT_H_
#define AGENTFLOW_WORKFLOW_SUB_AGENT_CONTEXT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "agentflow/core/cancel.h"
#include "agentflow/core/token_channel.h"
#include "agentflow/inference/chat_backend.h"
#include "agentflow/tools/gated_tool.h"

namespace agentflow::workflow {

// Reads the parent conversation's settled history for a fork delegation.
// Holds the conversation weakly: the provider is attached to the agent node's
// config for the whole run, while the conversation is created per Run.
struct ConversationHistoryProvider {
  std::weak_ptr<::agentflow::IConversation> conversation;

  absl::StatusOr<std::string> Export() const {
    auto conv = conversation.lock();
    if (!conv) {
      return absl::FailedPreconditionError("parent conversation unavailable");
    }
    return conv->ExportHistory();
  }
};

// Threaded through every delegate call. depth and root_invocation_id
// propagate down the call chain; root stays constant for the whole tree.
struct SubAgentContext {
  uint32_t depth = 0;
  std::string root_invocation_id;
  const CancelToken* parent_cancel = nullptr;
  std::shared_ptr<ToolInvocationGate> gate;
  std::unordered_map<std::string, ToolTier> tool_tiers;
  std::function<void(std::string_view, const ToolInvocationContext&,
                     std::string_view, std::string_view)> tool_event;

  // Optional per-invocation direct token stream. When set (and the child is
  // unconstrained), each generated text delta is pushed onto this channel as
  // it streams. The delegate tool creates one channel per call and drains it
  // up to the top-level stream — different sub-agents use different channels.
  TokenChannel* token_channel = nullptr;

  // Set by the delegate tool when the call asked for a forked child. The
  // child inherits the parent's settled history instead of a clean context.
  bool fork = false;
  // The parent conversation to export when `fork` is true. Null means the
  // caller had no conversation to offer (fork_unavailable).
  std::shared_ptr<ConversationHistoryProvider> parent_history;
};

}  // namespace agentflow::workflow
#endif
