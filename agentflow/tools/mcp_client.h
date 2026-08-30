// agentflow/tools/mcp_client.h
#ifndef AGENTFLOW_TOOLS_MCP_CLIENT_H_
#define AGENTFLOW_TOOLS_MCP_CLIENT_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/tool.h"  // ToolSchema
#include "mcp_spec.pb.h"

namespace agentflow::net { class IHttpClient; }

namespace agentflow::mcp {

// Abstract MCP client surface. The seam between the chosen client impl
// (currently the in-house McpClient below; gopher-mcp or another lib could
// drop in here) and everything downstream (pool, adapter, registry, tests).
class IMcpClient {
 public:
  virtual ~IMcpClient() = default;

  // Brings up the transport + performs the MCP `initialize` handshake.
  // No-op if already connected. Safe to call repeatedly. Calling Connect()
  // after a previous failure attempts to reconnect.
  virtual asio::awaitable<absl::Status> Connect() = 0;

  // `tools/list`. Returns each remote tool's ToolSchema (the `name` field is
  // the remote tool name, not yet namespaced).
  virtual asio::awaitable<absl::StatusOr<std::vector<ToolSchema>>>
  ListTools() = 0;

  // `tools/call`. `args_json` is forwarded as the request's `arguments`.
  // On success, returns the result content serialized as JSON. On error,
  // returns a non-OK status (the McpToolAdapter converts that into the
  // canonical MCP error shape so the LLM sees a uniform payload).
  // Cancellation aborts the in-flight RPC and sends a JSON-RPC
  // notifications/cancelled if the server is connected; the underlying
  // connection stays usable for subsequent calls.
  virtual asio::awaitable<absl::StatusOr<std::string>> CallTool(
      std::string_view name, std::string_view args_json,
      const CancelToken& cancel) = 0;

  // Tears down the transport. Pending CallTool awaitables resolve with
  // Cancelled. Safe to call multiple times.
  virtual void Shutdown() = 0;
};

// In-house MCP client. Supports the stdio and streamable-HTTP (HTTP_SSE)
// transports; WebSocket and TCP currently return Unimplemented (the impl is
// split so adding them only touches mcp_client.cc).
//
// Thread-safety: a single instance is intended to be driven from one
// io_context. CallTool() may be invoked concurrently from different
// coroutines on that io_context — JSON-RPC ids are unique per call and
// responses fan out via per-request channels.
class McpClient : public IMcpClient {
 public:
  static std::shared_ptr<McpClient> Create(
      proto::McpServerSpec spec, asio::io_context& io);

  // HTTP transports only. `http` non-null injects a client (tests); null makes
  // the impl build an HttpsClient itself. Ignored for STDIO.
  static std::shared_ptr<McpClient> Create(
      proto::McpServerSpec spec, asio::io_context& io,
      std::shared_ptr<net::IHttpClient> http);
  ~McpClient() override;

  McpClient(const McpClient&) = delete;
  McpClient& operator=(const McpClient&) = delete;

  asio::awaitable<absl::Status> Connect() override;
  asio::awaitable<absl::StatusOr<std::vector<ToolSchema>>>
  ListTools() override;
  asio::awaitable<absl::StatusOr<std::string>> CallTool(
      std::string_view name, std::string_view args_json,
      const CancelToken& cancel) override;
  void Shutdown() override;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;

  explicit McpClient(std::shared_ptr<Impl> impl);
};

}  // namespace agentflow::mcp

#endif  // AGENTFLOW_TOOLS_MCP_CLIENT_H_
