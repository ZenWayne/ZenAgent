#ifndef AGENTFLOW_TOOLS_INVOCATION_TOOL_VIEW_H_
#define AGENTFLOW_TOOLS_INVOCATION_TOOL_VIEW_H_

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agentflow/tools/gated_tool.h"
#include "agentflow/tools/tool_registry.h"

namespace agentflow {

// A tool slice assembled for one agent run or delegate invocation.
class InvocationToolView {
 public:
  InvocationToolView(const ToolRegistry& registry,
                     const std::vector<std::string>& declared_tools,
                     const std::unordered_map<std::string, ToolTier>& tiers,
                     ToolInvocationContext context,
                     std::shared_ptr<ToolInvocationGate> gate);
  std::string ExportToolsJson() const;
  asio::awaitable<std::string> Invoke(std::string_view name,
                                      std::string_view args_json,
                                      std::string_view call_id,
                                      const CancelToken& cancel);
  bool Has(std::string_view name) const;
  // Built-ins such as delegate are invocation-local too and never alter the
  // host registry.
  void Add(std::shared_ptr<Tool> tool);

 private:
  std::unordered_map<std::string, std::shared_ptr<Tool>> tools_;
};

}  // namespace agentflow
#endif
