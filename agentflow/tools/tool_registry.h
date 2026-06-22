// agentflow/tools/tool_registry.h
#ifndef AGENTFLOW_TOOLS_TOOL_REGISTRY_H_
#define AGENTFLOW_TOOLS_TOOL_REGISTRY_H_

#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <absl/status/status.h>
#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/tool.h"
#include "mcp_spec.pb.h"

namespace agentflow {

namespace mcp {
class IMcpClient;
class McpClientPool;
}  // namespace mcp

class ToolRegistry {
 public:
  // Default ctor — native tools only. AttachMcpServer is unavailable in this
  // mode and returns FailedPrecondition.
  ToolRegistry();

  // MCP-aware ctor. The io_context must outlive the registry; the McpClientPool
  // and any McpClient it creates pin themselves to this context.
  explicit ToolRegistry(asio::io_context& io);

  // MCP-aware ctor with an injected client factory (used by tests to plug in
  // a FakeMcpClient without subprocessing).
  ToolRegistry(asio::io_context& io,
               std::function<std::shared_ptr<mcp::IMcpClient>(
                   const proto::McpServerSpec&, asio::io_context&)>
                   client_factory);

  ~ToolRegistry();

  ToolRegistry(const ToolRegistry&) = delete;
  ToolRegistry& operator=(const ToolRegistry&) = delete;

  // Register a tool. Existing tool with the same name is REPLACED (matches
  // the P2 contract); use AttachMcpServer's skip-on-collision semantics for
  // discovery-driven registration.
  void Register(std::shared_ptr<Tool> tool);

  // Discovers a server's tools (tools/list), then registers one McpToolAdapter
  // per remote tool that passes the include/exclude filters.
  //
  // Failure semantics:
  //   - spec.lazy_start = false  →  transport + handshake + tools/list happen
  //                                 now; any failure returns non-OK and
  //                                 nothing is registered.
  //   - spec.lazy_start = true   →  transport setup is deferred to first
  //                                 Invoke. If tools/list fails at attach
  //                                 time anyway, we log + co_return OK with
  //                                 no tools registered, so the agent graph
  //                                 still starts.
  // Collision policy: a remote tool whose name is already registered (native
  // or earlier MCP) is SKIPPED with a warning — the local/earlier tool wins.
  asio::awaitable<absl::Status> AttachMcpServer(
      proto::McpServerSpec spec, std::string name_prefix = "");

  // Shuts down every MCP client this registry's pool has created. The MCP
  // adapters registered as tools stay in place but will return
  // FailedPrecondition on subsequent Invoke. Idempotent. Necessary for the
  // io_context-running thread to exit cleanly when all useful work is done:
  // each McpClient owns a detached ReadLoop coroutine that holds the io_context
  // alive until the transport is closed.
  void ShutdownMcp();

  asio::awaitable<std::string> Invoke(
      std::string_view name,
      std::string_view args_json,
      const CancelToken& cancel);

  // True iff a tool with the given name is currently registered.
  bool Has(std::string_view name) const;

  // OpenAI-compatible tools JSON array. Empty span → all registered tools.
  std::string ExportToolsJson(
      std::span<const std::string> tool_names) const;

 private:
  // Skip if a tool with the same name is already registered. Returns true
  // if registered, false if skipped (collision).
  bool TryRegisterIfAbsent(std::shared_ptr<Tool> tool);

  asio::io_context* io_ = nullptr;
  std::unique_ptr<mcp::McpClientPool> pool_;

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<Tool>> tools_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_TOOLS_TOOL_REGISTRY_H_
