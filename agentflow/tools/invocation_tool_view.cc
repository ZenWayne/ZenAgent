#include "agentflow/tools/invocation_tool_view.h"

#include "agentflow/tools/mcp_tool_adapter.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "absl/strings/str_cat.h"

namespace agentflow {
namespace {

// Clones the per-session MCP call context when this invocation carries callback
// routing metadata. MCP adapters in a registry are shared by identity; mutating
// the shared adapter would cross-wire simultaneous sessions.
std::shared_ptr<Tool> WithInvocationContext(
    const std::shared_ptr<Tool>& tool, const ToolInvocationContext& context) {
  if (context.session_id.empty()) return tool;
  if (auto mcp = std::dynamic_pointer_cast<mcp::McpToolAdapter>(tool)) {
    return mcp->WithCallContext(mcp::McpCallContext{context.session_id});
  }
  return tool;
}

}  // namespace

InvocationToolView::InvocationToolView(
    const ToolRegistry& registry, const std::vector<std::string>& declared_tools,
    const std::unordered_map<std::string, ToolTier>& tiers,
    ToolInvocationContext context, std::shared_ptr<ToolInvocationGate> gate)
    : context_(std::move(context)), gate_(std::move(gate)) {
  for (const auto& name : declared_tools) {
    auto source = registry.Find(name);
    if (!source) continue;
    const auto it = tiers.find(name);
    const ToolTier tier = it == tiers.end() ? ToolTier::kReadonly : it->second;
    // Blocked tools are absent even from the schema surface.
    if (tier == ToolTier::kBlocked) continue;
    tools_.emplace(name, std::make_shared<GatedTool>(
                             WithInvocationContext(source, context_), tier,
                             context_, gate_));
    registrations_.emplace(name, Registration{tier, std::move(source)});
  }
}

absl::Status InvocationToolView::AddFromRegistry(const ToolRegistry& registry,
                                                 std::string_view name,
                                                 ToolTier tier) {
  if (tier == ToolTier::kBlocked) {
    return absl::PermissionDeniedError(
        absl::StrCat("tool '", name, "' is blocked"));
  }
  auto source = registry.Find(std::string(name));
  if (!source) {
    return absl::NotFoundError(
        absl::StrCat("tool '", name, "' is not registered"));
  }
  const std::string key(name);
  // An unchanged re-registration is a no-op: replacing the wrapper would be
  // pure churn, and Add() built-ins are not registry tools so they never match.
  const auto existing = registrations_.find(key);
  if (existing != registrations_.end() && existing->second.tier == tier &&
      existing->second.source == source) {
    return absl::OkStatus();
  }
  auto wrapper = std::make_shared<GatedTool>(
      WithInvocationContext(source, context_), tier, context_, gate_);
  tools_[key] = std::move(wrapper);
  registrations_[key] = Registration{tier, std::move(source)};
  return absl::OkStatus();
}

std::string InvocationToolView::ExportToolsJson() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& [name, tool] : tools_) {
    const auto& s = tool->Schema();
    arr.push_back({{"type", "function"}, {"function", {{"name", s.name},
        {"description", s.description}, {"parameters", nlohmann::json::parse(s.params_json_schema)}}}});
  }
  return arr.dump();
}

asio::awaitable<std::string> InvocationToolView::Invoke(
    std::string_view name, std::string_view args_json, std::string_view call_id,
    const CancelToken& cancel) {
  // Taken before the first co_await: the callee is a member coroutine holding a
  // raw `this`, so the wrapper must outlive the call even if a concurrent
  // AddFromRegistry replaces or erases this name while the call is suspended.
  std::shared_ptr<Tool> tool;
  if (const auto it = tools_.find(std::string(name)); it != tools_.end()) {
    tool = it->second;
  }
  if (!tool) co_return R"({"error":"tool_not_allowed"})";
  co_return co_await tool->Invoke(args_json, call_id, cancel);
}

bool InvocationToolView::Has(std::string_view name) const {
  return tools_.contains(std::string(name));
}

void InvocationToolView::Add(std::shared_ptr<Tool> tool) {
  if (!tool) return;
  const std::string name = tool->Schema().name;
  // An invocation-local built-in replaces whatever was registered under this
  // name, so the registry registration no longer describes it.
  registrations_.erase(name);
  tools_[name] = std::move(tool);
}
}  // namespace agentflow
