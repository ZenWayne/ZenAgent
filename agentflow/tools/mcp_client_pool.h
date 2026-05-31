// agentflow/tools/mcp_client_pool.h
#ifndef AGENTFLOW_TOOLS_MCP_CLIENT_POOL_H_
#define AGENTFLOW_TOOLS_MCP_CLIENT_POOL_H_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <asio/io_context.hpp>

#include "agentflow/tools/mcp_client.h"
#include "mcp_spec.pb.h"

namespace agentflow::mcp {

// Owns one client per logical server, so multiple McpToolAdapters on the same
// server reuse the same transport / handshake / process. Key =
// (transport, command_or_url, args). include/exclude_tools, call_timeout_ms,
// and lazy_start are policy on top of the same connection and do NOT affect
// keying.
//
// The default factory creates the in-house McpClient. Tests inject a custom
// factory to return a FakeMcpClient (or any IMcpClient).
class McpClientPool {
 public:
  using ClientFactory = std::function<std::shared_ptr<IMcpClient>(
      const proto::McpServerSpec&, asio::io_context&)>;

  // Default factory: McpClient::Create.
  explicit McpClientPool(asio::io_context& io);
  // Inject a custom factory (for tests).
  McpClientPool(asio::io_context& io, ClientFactory factory);
  ~McpClientPool();

  McpClientPool(const McpClientPool&) = delete;
  McpClientPool& operator=(const McpClientPool&) = delete;

  // Returns the existing client for `spec` or creates one via the factory.
  std::shared_ptr<IMcpClient> GetOrCreate(const proto::McpServerSpec& spec);

  // Shuts down every client (cancels in-flight calls). Safe to call repeatedly.
  void Clear();

  // For tests.
  std::size_t size() const;

 private:
  static std::string CanonicalKey(const proto::McpServerSpec& spec);

  asio::io_context& io_;
  ClientFactory factory_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<IMcpClient>> clients_;
};

}  // namespace agentflow::mcp

#endif  // AGENTFLOW_TOOLS_MCP_CLIENT_POOL_H_
