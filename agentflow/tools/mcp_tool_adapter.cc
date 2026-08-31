// agentflow/tools/mcp_tool_adapter.cc
#include "agentflow/tools/mcp_tool_adapter.h"

#include <memory>
#include <string>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

namespace agentflow::mcp {

namespace {

using json = nlohmann::json;

std::string McpErrorJson(std::string_view message) {
  json err = {
      {"isError", true},
      {"content", json::array({json::object({
                       {"type", "text"},
                       {"text", std::string(message)},
                   })})},
  };
  return err.dump();
}

}  // namespace

McpToolAdapter::McpToolAdapter(std::shared_ptr<IMcpClient> client,
                               std::string remote_tool_name,
                               ToolSchema cached_schema,
                               std::chrono::milliseconds call_timeout)
    : client_(std::move(client)),
      remote_name_(std::move(remote_tool_name)),
      schema_(std::move(cached_schema)),
      timeout_(call_timeout) {}

asio::awaitable<std::string> McpToolAdapter::Invoke(
    std::string_view args_json, std::string_view /*tool_call_id*/,
    const CancelToken& cancel) {
  using namespace asio::experimental::awaitable_operators;

  if (cancel.IsCancelled()) {
    co_return McpErrorJson("tool error: cancelled before invoke");
  }

  // No timeout: just delegate.
  if (timeout_ == std::chrono::milliseconds{0}) {
    auto result = co_await client_->CallTool(remote_name_, args_json, cancel);
    if (!result.ok()) {
      co_return McpErrorJson("tool error: " +
                             std::string(result.status().message()));
    }
    co_return *std::move(result);
  }

  // Race the call against a steady_timer.
  auto exec = co_await asio::this_coro::executor;
  asio::steady_timer timer(exec, timeout_);
  auto outcome = co_await (
      client_->CallTool(remote_name_, args_json, cancel) ||
      timer.async_wait(asio::use_awaitable));
  if (outcome.index() == 1) {
    // Timer won — best-effort: the in-flight call will be cleaned up when
    // its CancelToken is flipped by the caller or on Shutdown. The std::variant
    // outcome holds Status* from the CallTool branch only when index == 0.
    co_return McpErrorJson("tool error: timeout after " +
                           std::to_string(timeout_.count()) + "ms");
  }
  auto result = std::move(std::get<0>(outcome));
  if (!result.ok()) {
    co_return McpErrorJson("tool error: " +
                           std::string(result.status().message()));
  }
  co_return *std::move(result);
}

}  // namespace agentflow::mcp
