// agentflow/tools/mcp_tool_adapter.h
#ifndef AGENTFLOW_TOOLS_MCP_TOOL_ADAPTER_H_
#define AGENTFLOW_TOOLS_MCP_TOOL_ADAPTER_H_

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/mcp_client.h"
#include "agentflow/tools/tool.h"

namespace agentflow::mcp {

// Wraps one remote MCP tool behind the Tool interface so it's
// indistinguishable from a NativeFnTool to the agent layer.
//
// On underlying-status failure (transport, server error, timeout, cancel)
// Invoke() returns the MCP-canonical error payload as JSON:
//     {"isError": true,
//      "content": [{"type": "text", "text": "<message>"}]}
// so the LLM sees the same shape as a successful tool result and can reason
// about the failure in-prompt.
class McpToolAdapter : public Tool {
 public:
  McpToolAdapter(std::shared_ptr<IMcpClient> client,
                 std::string remote_tool_name,
                 ToolSchema cached_schema,
                 std::chrono::milliseconds call_timeout =
                     std::chrono::milliseconds{0});

  const ToolSchema& Schema() const override { return schema_; }

  asio::awaitable<std::string> Invoke(std::string_view args_json,
                                      std::string_view tool_call_id,
                                      const CancelToken& cancel) override;

 private:
  std::shared_ptr<IMcpClient> client_;
  std::string remote_name_;
  ToolSchema schema_;
  std::chrono::milliseconds timeout_;
};

}  // namespace agentflow::mcp

#endif  // AGENTFLOW_TOOLS_MCP_TOOL_ADAPTER_H_
