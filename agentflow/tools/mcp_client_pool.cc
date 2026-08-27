// agentflow/tools/mcp_client_pool.cc
#include "agentflow/tools/mcp_client_pool.h"

#include <algorithm>
#include <utility>
#include <vector>

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
  // Identifying fields: transport, command_or_url, args, AND headers (e.g. an
  // Authorization bearer -- see proto/mcp_spec.proto) all participate in
  // keying. Two AttachMcpServer calls that differ only in headers must get
  // DIFFERENT clients, or the second caller's requests would silently go out
  // under the first caller's credentials. Filters and per-call policy
  // (include_tools, exclude_tools, call_timeout_ms, lazy_start) still do NOT
  // affect keying: two attach calls with different include_tools but the
  // same target server + credentials must share the same connection.
  std::string key;
  key.reserve(64);
  key.append(std::to_string(static_cast<int>(spec.transport())));
  key.push_back('\x1f');  // unit separator
  key.append(spec.command_or_url());
  for (const auto& a : spec.args()) {
    key.push_back('\x1f');
    key.append(a);
  }

  // proto::Map iteration order is UNSPECIFIED: sort entries by (key, then
  // value) before appending, or two semantically-identical specs could
  // produce different keys depending on map bucket layout -- dedup would
  // then only work intermittently, which is worse than the bug this change
  // fixes. std::pair's default operator< gives us (key, then value) ordering
  // for free.
  std::vector<std::pair<std::string, std::string>> headers(
      spec.headers().begin(), spec.headers().end());
  std::sort(headers.begin(), headers.end());

  // A second, distinct separator ('\x1e', record separator) marks the start
  // of the headers section, and each entry is length-prefixed
  // (netstring-style: "<byte-length>:<bytes>") rather than joined with a bare
  // '\x1f' like command_or_url/args above. Both guard against the same
  // failure mode: a header key or value that happens to itself contain '\x1f'
  // could otherwise splice with an adjacent field and forge a collision with
  // a differently-shaped header set. Length-prefixing makes each field's
  // boundary exact regardless of its byte content -- there is never a need to
  // search for a delimiter inside attacker/user-supplied header data (e.g. a
  // bearer token), so an embedded separator byte can't misparse the key.
  key.push_back('\x1e');
  for (const auto& [k, v] : headers) {
    key.push_back('\x1f');
    key.append(std::to_string(k.size()));
    key.push_back(':');
    key.append(k);
    key.push_back('\x1f');
    key.append(std::to_string(v.size()));
    key.push_back(':');
    key.append(v);
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
