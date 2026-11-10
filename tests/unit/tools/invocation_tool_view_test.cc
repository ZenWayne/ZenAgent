#include "agentflow/tools/invocation_tool_view.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/tools/mcp_client.h"
#include "agentflow/tools/mcp_tool_adapter.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool_registry.h"

namespace agentflow {
namespace {

class FixedGate final : public ToolInvocationGate {
 public:
  explicit FixedGate(ToolGateDecision decision) : decision_(std::move(decision)) {}
  asio::awaitable<ToolGateDecision> Decide(
      std::string_view, std::string_view, const ToolInvocationContext&,
      std::string_view, const CancelToken&) override {
    ++calls;
    co_return decision_;
  }
  int calls = 0;

 private:
  ToolGateDecision decision_;
};

// Records the trusted call context each MCP invocation carries.
class RecordingMcpClient final : public mcp::IMcpClient {
 public:
  asio::awaitable<absl::Status> Connect() override {
    co_return absl::OkStatus();
  }
  asio::awaitable<absl::StatusOr<std::vector<ToolSchema>>> ListTools() override {
    co_return std::vector<ToolSchema>{};
  }
  asio::awaitable<absl::StatusOr<std::string>> CallTool(
      std::string_view, std::string_view, const CancelToken&,
      const mcp::McpCallContext& context) override {
    session_ids.push_back(context.session_id);
    co_return std::string("mcp-ok");
  }
  void Shutdown() override {}

  std::vector<std::string> session_ids;
};

std::string InvokeView(InvocationToolView& view, std::string_view name,
                       asio::io_context& io) {
  auto future = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await view.Invoke(name, "{}", "call-1", CancelToken{});
      },
      asio::use_future);
  io.run();
  io.restart();
  return future.get();
}

std::shared_ptr<NativeFnTool> NativeCounter(int* calls) {
  return std::make_shared<NativeFnTool>(
      ToolSchema{"write", "", "{}"},
      [calls](auto, auto, auto) -> asio::awaitable<std::string> {
        ++*calls;
        co_return "ran";
      });
}

TEST(InvocationToolViewTest, AddFromRegistryGatesConfirmTools) {
  ToolRegistry registry;
  int invoked = 0;
  registry.Register(NativeCounter(&invoked));
  auto gate =
      std::make_shared<FixedGate>(ToolGateDecision{false, "rejected_by_user", {}});
  InvocationToolView view(registry, {}, {}, {}, gate);

  ASSERT_TRUE(view.AddFromRegistry(registry, "write", ToolTier::kConfirm).ok());
  EXPECT_TRUE(view.Has("write"));
  EXPECT_NE(view.ExportToolsJson().find("\"write\""), std::string::npos);

  asio::io_context io;
  EXPECT_EQ(InvokeView(view, "write", io), R"({"error":"rejected_by_user"})");
  EXPECT_EQ(gate->calls, 1);
  EXPECT_EQ(invoked, 0);
}

TEST(InvocationToolViewTest, AddFromRegistryLetsApprovedConfirmToolsRun) {
  ToolRegistry registry;
  int invoked = 0;
  registry.Register(NativeCounter(&invoked));
  auto gate = std::make_shared<FixedGate>(ToolGateDecision{true, {}, {}});
  InvocationToolView view(registry, {}, {}, {}, gate);

  ASSERT_TRUE(view.AddFromRegistry(registry, "write", ToolTier::kConfirm).ok());
  asio::io_context io;
  EXPECT_EQ(InvokeView(view, "write", io), "ran");
  EXPECT_EQ(gate->calls, 1);
  EXPECT_EQ(invoked, 1);
}

TEST(InvocationToolViewTest, AddFromRegistryRejectsBlockedAndUnknownTools) {
  ToolRegistry registry;
  int invoked = 0;
  registry.Register(NativeCounter(&invoked));
  InvocationToolView view(registry, {}, {}, {}, nullptr);

  const absl::Status blocked =
      view.AddFromRegistry(registry, "write", ToolTier::kBlocked);
  EXPECT_EQ(blocked.code(), absl::StatusCode::kPermissionDenied);
  EXPECT_FALSE(view.Has("write"));
  EXPECT_EQ(view.ExportToolsJson().find("write"), std::string::npos);

  const absl::Status missing =
      view.AddFromRegistry(registry, "missing", ToolTier::kReadonly);
  EXPECT_EQ(missing.code(), absl::StatusCode::kNotFound);
  EXPECT_FALSE(view.Has("missing"));
}

TEST(InvocationToolViewTest, AddFromRegistryClonesTheMcpCallContext) {
  auto client = std::make_shared<RecordingMcpClient>();
  ToolRegistry registry;
  registry.Register(std::make_shared<mcp::McpToolAdapter>(
      client, "remote", ToolSchema{"remote", "", "{}"}));
  ToolInvocationContext context;
  context.session_id = "sess-42";
  InvocationToolView view(registry, {}, {}, context, nullptr);

  ASSERT_TRUE(view.AddFromRegistry(registry, "remote", ToolTier::kReadonly).ok());
  asio::io_context io;
  EXPECT_EQ(InvokeView(view, "remote", io), "mcp-ok");
  ASSERT_EQ(client->session_ids.size(), 1u);
  EXPECT_EQ(client->session_ids[0], "sess-42");

  // The shared registry adapter keeps its empty context: only this view's clone
  // carries the session, so simultaneous sessions cannot cross-wire.
  auto shared = registry.Find("remote");
  ASSERT_NE(shared, nullptr);
  auto future = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await shared->Invoke("{}", "call-2", CancelToken{});
      },
      asio::use_future);
  io.run();
  EXPECT_EQ(future.get(), "mcp-ok");
  ASSERT_EQ(client->session_ids.size(), 2u);
  EXPECT_EQ(client->session_ids[1], "");
}

}  // namespace
}  // namespace agentflow
