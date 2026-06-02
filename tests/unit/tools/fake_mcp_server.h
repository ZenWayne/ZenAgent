// tests/unit/tools/fake_mcp_server.h
//
// In-process IMcpClient impl driven by std::function hooks. Lets us exercise
// McpToolAdapter / McpClientPool / ToolRegistry::AttachMcpServer without
// subprocess plumbing or a real MCP server.
//
// (Named `fake_mcp_server` per the plan; technically it fakes the *client*
// side, but it stands in for what a real server-backed client would return.)

#ifndef AGENTFLOW_TESTS_UNIT_TOOLS_FAKE_MCP_SERVER_H_
#define AGENTFLOW_TESTS_UNIT_TOOLS_FAKE_MCP_SERVER_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/mcp_client.h"
#include "agentflow/tools/tool.h"

namespace agentflow {
namespace testing {

class FakeMcpClient : public mcp::IMcpClient {
 public:
  explicit FakeMcpClient(asio::io_context& io) : io_(io) {}

  // Hooks. When set, used in preference to the default values below.
  std::function<absl::Status()> on_connect;
  std::function<absl::StatusOr<std::vector<ToolSchema>>()> on_list_tools;
  std::function<absl::StatusOr<std::string>(std::string_view name,
                                            std::string_view args_json)>
      on_call_tool;
  std::function<void()> on_shutdown;

  // Optional artificial delay inside CallTool (default 0 = no delay). Used by
  // the timeout test.
  std::chrono::milliseconds call_delay{0};

  // Counters / state, all manipulated only on the io_context thread.
  std::atomic<int> connect_calls{0};
  std::atomic<int> list_calls{0};
  std::atomic<int> call_calls{0};
  std::atomic<int> shutdown_calls{0};
  std::atomic<bool> connected{false};

  asio::awaitable<absl::Status> Connect() override {
    ++connect_calls;
    absl::Status s = on_connect ? on_connect() : absl::OkStatus();
    if (s.ok()) connected = true;
    co_return s;
  }

  asio::awaitable<absl::StatusOr<std::vector<ToolSchema>>>
  ListTools() override {
    ++list_calls;
    if (on_list_tools) co_return on_list_tools();
    co_return std::vector<ToolSchema>{};
  }

  asio::awaitable<absl::StatusOr<std::string>> CallTool(
      std::string_view name, std::string_view args_json,
      const CancelToken& /*cancel*/) override {
    ++call_calls;
    if (call_delay > std::chrono::milliseconds{0}) {
      asio::steady_timer t(io_, call_delay);
      co_await t.async_wait(asio::use_awaitable);
    }
    if (on_call_tool) co_return on_call_tool(name, args_json);
    co_return std::string{};
  }

  void Shutdown() override {
    ++shutdown_calls;
    connected = false;
    if (on_shutdown) on_shutdown();
  }

 private:
  asio::io_context& io_;
};

}  // namespace testing
}  // namespace agentflow

#endif  // AGENTFLOW_TESTS_UNIT_TOOLS_FAKE_MCP_SERVER_H_
