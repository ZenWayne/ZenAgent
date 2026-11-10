#include "agentflow/tools/invocation_tool_view.h"

#include "agentflow/tools/mcp_tool_adapter.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "absl/strings/str_cat.h"

namespace agentflow {
InvocationToolView::InvocationToolView(
    const ToolRegistry& registry, const std::vector<std::string>& declared_tools,
    const std::unordered_map<std::string, ToolTier>& tiers,
    ToolInvocationContext context, std::shared_ptr<ToolInvocationGate> gate)
    : context_(std::move(context)), gate_(std::move(gate)) {
  for (const auto& name : declared_tools) {
    auto inner = registry.Find(name);
    if (!inner) continue;
    // MCP adapters in a registry are shared by identity. Clone only the
    // lightweight adapter when this invocation has callback routing metadata;
    // mutating the shared adapter would cross-wire simultaneous sessions.
    if (!context_.session_id.empty()) {
      if (auto mcp = std::dynamic_pointer_cast<mcp::McpToolAdapter>(inner)) {
        inner = mcp->WithCallContext(mcp::McpCallContext{context_.session_id});
      }
    }
    const auto it = tiers.find(name);
    const ToolTier tier = it == tiers.end() ? ToolTier::kReadonly : it->second;
    // Blocked tools are absent even from the schema surface.
    if (tier == ToolTier::kBlocked) continue;
    tools_.emplace(name, std::make_shared<GatedTool>(inner, tier, context_, gate_));
  }
}

absl::Status InvocationToolView::AddFromRegistry(const ToolRegistry& registry,
                                                 std::string_view name,
                                                 ToolTier tier) {
  if (tier == ToolTier::kBlocked) {
    return absl::PermissionDeniedError(
        absl::StrCat("tool '", name, "' is blocked"));
  }
  auto inner = registry.Find(std::string(name));
  if (!inner) {
    return absl::NotFoundError(
        absl::StrCat("tool '", name, "' is not registered"));
  }
  if (!context_.session_id.empty()) {
    if (auto mcp = std::dynamic_pointer_cast<mcp::McpToolAdapter>(inner)) {
      inner = mcp->WithCallContext(mcp::McpCallContext{context_.session_id});
    }
  }
  tools_[std::string(name)] = std::make_shared<GatedTool>(inner, tier, context_, gate_);
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
  auto it = tools_.find(std::string(name));
  if (it == tools_.end()) co_return R"({"error":"tool_not_allowed"})";
  co_return co_await it->second->Invoke(args_json, call_id, cancel);
}

bool InvocationToolView::Has(std::string_view name) const {
  return tools_.contains(std::string(name));
}

void InvocationToolView::Add(std::shared_ptr<Tool> tool) {
  if (tool) tools_[tool->Schema().name] = std::move(tool);
}
}  // namespace agentflow
