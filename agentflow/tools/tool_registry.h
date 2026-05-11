// agentflow/tools/tool_registry.h
#ifndef AGENTFLOW_TOOLS_TOOL_REGISTRY_H_
#define AGENTFLOW_TOOLS_TOOL_REGISTRY_H_

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/tool.h"

namespace agentflow {

class ToolRegistry {
 public:
  void Register(std::shared_ptr<Tool> tool);

  asio::awaitable<std::string> Invoke(
      std::string_view name,
      std::string_view args_json,
      const CancelToken& cancel);

  // Returns OpenAI-compatible tools JSON array for LLM function calling.
  std::string ExportToolsJson(
      std::span<const std::string> tool_names) const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<Tool>> tools_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_TOOLS_TOOL_REGISTRY_H_
