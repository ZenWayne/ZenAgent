// agentflow/tools/mcp_client_pool.cc
#include "agentflow/tools/mcp_client_pool.h"

#include <utility>

namespace agentflow::mcp {

namespace {
std::shared_ptr<IMcpClient> DefaultFactory(const proto::McpServerSpec& spec,
                                           asio::io_context& io) {
  return McpClient::Create(spec, io);
}
}  // namespace

McpClientPool::McpClientPool(asio::io_context& io)
    : io_(io), factory_(&DefaultFactory) {}

McpClientPool::McpClientPool(asio::io_context& io, ClientFactory factory)
    : io_(io), factory_(std::move(factory)) {}

McpClientPool::~McpClientPool() { Clear(); }

std::string McpClientPool::CanonicalKey(const proto::McpServerSpec& spec) {
  // Identifying fields only. Filters and per-call policy don't affect keying:
  // two attach_calls with different include_tools but the same target server
  // must share the same connection.
  std::string key;
  key.reserve(64);
  key.append(std::to_string(static_cast<int>(spec.transport())));
  key.push_back('\x1f');  // unit separator
  key.append(spec.command_or_url());
  for (const auto& a : spec.args()) {
    key.push_back('\x1f');
    key.append(a);
  }
  return key;
}

std::shared_ptr<IMcpClient> McpClientPool::GetOrCreate(
    const proto::McpServerSpec& spec) {
  const std::string key = CanonicalKey(spec);
  std::lock_guard<std::mutex> lk(mu_);
  if (auto it = clients_.find(key); it != clients_.end()) {
    return it->second;
  }
  auto client = factory_(spec, io_);
  clients_.emplace(key, client);
  return client;
}

void McpClientPool::Clear() {
  std::unordered_map<std::string, std::shared_ptr<IMcpClient>> taken;
  {
    std::lock_guard<std::mutex> lk(mu_);
    taken.swap(clients_);
  }
  for (auto& [_, client] : taken) {
    client->Shutdown();
  }
}

std::size_t McpClientPool::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return clients_.size();
}

}  // namespace agentflow::mcp
