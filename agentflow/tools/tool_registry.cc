// agentflow/tools/tool_registry.cc
#include "agentflow/tools/tool_registry.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include <absl/strings/str_cat.h>

#include "agentflow/core/errors.h"
#include "agentflow/tools/mcp_client.h"
#include "agentflow/tools/mcp_client_pool.h"
#include "agentflow/tools/mcp_tool_adapter.h"

namespace agentflow {
namespace {

nlohmann::json SchemaToJson(const ToolSchema& schema) {
  return {
      {"type", "function"},
      {"function",
       {
           {"name", schema.name},
           {"description", schema.description},
           {"parameters", nlohmann::json::parse(schema.params_json_schema)},
       }},
  };
}

}  // namespace

ToolRegistry::ToolRegistry() = default;

ToolRegistry::ToolRegistry(asio::io_context& io)
    : io_(&io), pool_(std::make_unique<mcp::McpClientPool>(io)) {}

ToolRegistry::ToolRegistry(
    asio::io_context& io,
    std::function<std::shared_ptr<mcp::IMcpClient>(
        const proto::McpServerSpec&, asio::io_context&)>
        client_factory)
    : io_(&io),
      pool_(std::make_unique<mcp::McpClientPool>(io,
                                                 std::move(client_factory))) {}

ToolRegistry::~ToolRegistry() = default;

void ToolRegistry::ShutdownMcp() {
  if (pool_) pool_->Clear();
}

void ToolRegistry::Register(std::shared_ptr<Tool> tool) {
  std::lock_guard<std::mutex> lk(mu_);
  tools_[tool->Schema().name] = std::move(tool);
}

bool ToolRegistry::Has(std::string_view name) const {
  std::lock_guard<std::mutex> lk(mu_);
  return tools_.find(std::string(name)) != tools_.end();
}

bool ToolRegistry::TryRegisterIfAbsent(std::shared_ptr<Tool> tool) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto& name = tool->Schema().name;
  if (tools_.find(name) != tools_.end()) return false;
  tools_.emplace(name, std::move(tool));
  return true;
}

asio::awaitable<absl::Status> ToolRegistry::AttachMcpServer(
    proto::McpServerSpec spec, std::string name_prefix) {
  if (pool_ == nullptr) {
    co_return absl::FailedPreconditionError(
        "AttachMcpServer requires the io_context-aware ToolRegistry ctor");
  }

  // Snapshot policy fields before std::move'ing the spec into the pool.
  const std::unordered_set<std::string> include_set(
      spec.include_tools().begin(), spec.include_tools().end());
  const std::unordered_set<std::string> exclude_set(
      spec.exclude_tools().begin(), spec.exclude_tools().end());
  const auto timeout = std::chrono::milliseconds(spec.call_timeout_ms());
  const bool lazy = spec.lazy_start();

  auto client = pool_->GetOrCreate(spec);

  if (!lazy) {
    auto s = co_await client->Connect();
    if (!s.ok()) {
      std::fprintf(stderr,
                   "[ToolRegistry] AttachMcpServer eager Connect failed "
                   "for '%s': %s\n",
                   spec.command_or_url().c_str(),
                   std::string(s.message()).c_str());
      co_return s;
    }
  }

  auto list = co_await client->ListTools();
  if (!list.ok()) {
    std::fprintf(stderr,
                 "[ToolRegistry] tools/list failed for '%s': %s%s\n",
                 spec.command_or_url().c_str(),
                 std::string(list.status().message()).c_str(),
                 lazy ? " (lazy_start=true, returning OK so the graph runs"
                        " without these tools)"
                      : "");
    if (lazy) co_return absl::OkStatus();
    co_return list.status();
  }

  int registered = 0;
  int skipped_collision = 0;
  int skipped_filtered = 0;
  for (auto& schema : *list) {
    // Filters match the RAW remote name (before namespacing).
    if (!include_set.empty() &&
        include_set.find(schema.name) == include_set.end()) {
      ++skipped_filtered;
      continue;
    }
    if (exclude_set.find(schema.name) != exclude_set.end()) {
      ++skipped_filtered;
      continue;
    }

    const std::string remote_name = schema.name;
    ToolSchema registered_schema = schema;  // copy; mutate the registry-facing name
    if (!name_prefix.empty()) {
      registered_schema.name = absl::StrCat(name_prefix, ".", remote_name);
    }
    auto adapter = std::make_shared<mcp::McpToolAdapter>(
        client, remote_name, registered_schema, timeout);
    if (TryRegisterIfAbsent(adapter)) {
      ++registered;
    } else {
      ++skipped_collision;
      std::fprintf(stderr,
                   "[ToolRegistry] skipping MCP tool '%s' from '%s' — name "
                   "already registered (local/earlier wins)\n",
                   registered_schema.name.c_str(),
                   spec.command_or_url().c_str());
    }
  }
  std::fprintf(stderr,
               "[ToolRegistry] attached MCP server '%s': registered=%d, "
               "skipped_collision=%d, skipped_filtered=%d\n",
               spec.command_or_url().c_str(), registered, skipped_collision,
               skipped_filtered);
  co_return absl::OkStatus();
}

asio::awaitable<std::string> ToolRegistry::Invoke(std::string_view name,
                                                  std::string_view args_json,
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
