// agentflow/tools/mcp_client_pool.h
#ifndef AGENTFLOW_TOOLS_MCP_CLIENT_POOL_H_
#define AGENTFLOW_TOOLS_MCP_CLIENT_POOL_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <asio/io_context.hpp>

#include "agentflow/tools/mcp_client.h"
#include "mcp_spec.pb.h"

namespace agentflow::mcp {

// Owns one McpClient per logical server, so multiple McpToolAdapters on the
// same server reuse the same transport / handshake / process. The key is the
// (transport, command_or_url, args) triple — include/exclude_tools,
// call_timeout_ms, and lazy_start are policy on top of the same connection
// and do NOT affect keying.
class McpClientPool {
 public:
  explicit McpClientPool(asio::io_context& io);
  ~McpClientPool();

  McpClientPool(const McpClientPool&) = delete;
  McpClientPool& operator=(const McpClientPool&) = delete;

  // Returns the existing client for `spec` or creates one. Thread-safe.
  std::shared_ptr<McpClient> GetOrCreate(const proto::McpServerSpec& spec);

  // Shuts down every client (cancels in-flight calls). Safe to call repeatedly.
  void Clear();

  // For tests.
  std::size_t size() const;

 private:
  static std::string CanonicalKey(const proto::McpServerSpec& spec);

  asio::io_context& io_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<McpClient>> clients_;
};

}  // namespace agentflow::mcp

#endif  // AGENTFLOW_TOOLS_MCP_CLIENT_POOL_H_
