// agentflow/tools/tool.h
#ifndef AGENTFLOW_TOOLS_TOOL_H_
#define AGENTFLOW_TOOLS_TOOL_H_

#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"

namespace agentflow {

struct ToolSchema {
  std::string name;
  std::string description;
  std::string params_json_schema;  // JSON Schema for arguments
};

class Tool {
 public:
  virtual ~Tool() = default;
  virtual const ToolSchema& Schema() const = 0;
  virtual asio::awaitable<std::string> Invoke(
      std::string_view args_json,
      const CancelToken& cancel) = 0;
};

}  // namespace agentflow
#endif  // AGENTFLOW_TOOLS_TOOL_H_
