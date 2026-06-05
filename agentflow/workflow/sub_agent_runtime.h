#ifndef AGENTFLOW_WORKFLOW_SUB_AGENT_RUNTIME_H_
#define AGENTFLOW_WORKFLOW_SUB_AGENT_RUNTIME_H_

#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "agentflow/core/event.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/sub_agent_context.h"
#include "agentflow/workflow/workflow.h"

namespace agentflow {
class LiteRtLmEngine;
}

namespace asio { class io_context; }

namespace agentflow::workflow {

class SubAgentRuntime {
 public:
  // Skeleton ctor (Task 4.4): hermetic-only depth/trace, no engine.
  SubAgentRuntime(std::shared_ptr<Workflow> wf,
                   const ToolRegistry& host_tools,
                   EventEmitter& emit);

  // Real-LLM ctor (Task 4.5): includes engine + io_context. Pointer/ref
  // members are nullable for skeleton-only call sites.
  SubAgentRuntime(std::shared_ptr<Workflow> wf,
                   const ToolRegistry& host_tools,
                   EventEmitter& emit,
                   std::shared_ptr<::agentflow::LiteRtLmEngine> engine,
                   ::asio::io_context& io);

  // Synchronous sub-agent run. Returns a JSON value (typically a string,
  // or an error object {"error":"<kind>",...}). NEVER throws.
  [[nodiscard]] nlohmann::ordered_json RunSync(std::string_view parent_agent,
                                                  std::string_view child_agent,
                                                  std::string_view goal,
                                                  const SubAgentContext& ctx);

 private:
  std::shared_ptr<Workflow> wf_;
  const ToolRegistry& host_tools_;
  EventEmitter& emit_;
  std::shared_ptr<::agentflow::LiteRtLmEngine> engine_;
  ::asio::io_context* io_ = nullptr;
};

}  // namespace agentflow::workflow
#endif
