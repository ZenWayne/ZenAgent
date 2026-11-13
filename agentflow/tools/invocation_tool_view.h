#ifndef AGENTFLOW_TOOLS_INVOCATION_TOOL_VIEW_H_
#define AGENTFLOW_TOOLS_INVOCATION_TOOL_VIEW_H_

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
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
  // The selected wrapper is copied into the coroutine before it awaits, so a
  // concurrent AddFromRegistry (a skill load registering the same name) drops
  // the map's reference while the in-flight call keeps the wrapper — and its
  // inner tool — alive.
  asio::awaitable<std::string> Invoke(std::string_view name,
                                      std::string_view args_json,
                                      std::string_view call_id,
                                      const CancelToken& cancel);
  bool Has(std::string_view name) const;
  // Built-ins such as delegate are invocation-local too and never alter the
  // host registry.
  void Add(std::shared_ptr<Tool> tool);
  // Registers a registry tool exactly like the constructor does: it clones the
  // per-session MCP call context and wraps the tool in GatedTool so kConfirm
  // still reaches the confirmation gate. kBlocked is rejected and nothing is
  // registered. Re-registering the same registry tool at the same tier is a
  // no-op, so a repeated load does not churn a wrapper that is already correct.
  // Use Add() only for invocation-local built-ins.
  absl::Status AddFromRegistry(const ToolRegistry& registry,
                               std::string_view name, ToolTier tier);

 private:
  // What a name was wrapped from, kept so AddFromRegistry can recognise an
  // unchanged re-registration. `source` is the registry tool before the MCP
  // call-context clone, so identity survives the per-session cloning.
  struct Registration {
    ToolTier tier = ToolTier::kReadonly;
    std::shared_ptr<Tool> source;
  };

  ToolInvocationContext context_;
  std::shared_ptr<ToolInvocationGate> gate_;
  std::unordered_map<std::string, std::shared_ptr<Tool>> tools_;
  std::unordered_map<std::string, Registration> registrations_;
};

}  // namespace agentflow
#endif
