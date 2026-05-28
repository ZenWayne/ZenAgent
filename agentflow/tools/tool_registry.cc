// agentflow/tools/tool_registry.cc
#include "agentflow/tools/tool_registry.h"

#include <stdexcept>

#include "agentflow/core/errors.h"

namespace agentflow {
namespace {

nlohmann::json SchemaToJson(const ToolSchema& schema) {
  return {
    {"type", "function"},
    {"function", {
      {"name", schema.name},
      {"description", schema.description},
      {"parameters", nlohmann::json::parse(schema.params_json_schema)},
    }},
  };
}

}  // namespace

void ToolRegistry::Register(std::shared_ptr<Tool> tool) {
  std::lock_guard<std::mutex> lk(mu_);
  tools_[tool->Schema().name] = std::move(tool);
}

asio::awaitable<std::string> ToolRegistry::Invoke(
    std::string_view name, std::string_view args_json,
    const CancelToken& cancel) {
  std::shared_ptr<Tool> tool;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tools_.find(std::string(name));
    if (it == tools_.end()) {
      throw AgentflowError("Tool not found: " + std::string(name));
    }
    tool = it->second;
  }
  co_return co_await tool->Invoke(args_json, cancel);
}

std::string ToolRegistry::ExportToolsJson(
    std::span<const std::string> tool_names) const {
  nlohmann::json arr = nlohmann::json::array();
  std::lock_guard<std::mutex> lk(mu_);

  if (tool_names.empty()) {
    // Empty span means export all registered tools.
    for (const auto& [name, tool] : tools_) {
      arr.push_back(SchemaToJson(tool->Schema()));
    }
  } else {
    for (const auto& name : tool_names) {
      auto it = tools_.find(name);
      if (it != tools_.end()) {
        arr.push_back(SchemaToJson(it->second->Schema()));
      }
    }
  }
  return arr.dump();
}

}  // namespace agentflow
