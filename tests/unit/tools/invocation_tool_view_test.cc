#include "agentflow/tools/invocation_tool_view.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
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

// Parks its coroutine on a shared timer so a re-registration can land while the
// call is in flight. `started` counts how often this instance actually ran.
class SuspendingTool final : public Tool {
 public:
  SuspendingTool(std::shared_ptr<asio::steady_timer> release, std::string label)
      : release_(std::move(release)), label_(std::move(label)) {}

  const ToolSchema& Schema() const override { return schema_; }

  asio::awaitable<std::string> Invoke(std::string_view, std::string_view,
                                      const CancelToken&) override {
    ++started;
    // Cancelling the timer is how the test releases the call, so the aborted
    // wait is the expected path, not a failure.
    auto [error] = co_await release_->async_wait(
        asio::as_tuple(asio::use_awaitable));
    static_cast<void>(error);
    co_return label_;
  }

  int started = 0;

 private:
  ToolSchema schema_{"write", "", "{}"};
  std::shared_ptr<asio::steady_timer> release_;
  std::string label_;
};

TEST(InvocationToolViewTest, ReRegistrationKeepsAnInFlightCallAlive) {
  asio::io_context io;
  auto release = std::make_shared<asio::steady_timer>(io);
  release->expires_after(std::chrono::hours(1));
  ToolRegistry registry;
  auto first = std::make_shared<SuspendingTool>(release, "first");
  registry.Register(first);
  // The emit callback is the second dangling access: GatedTool::Invoke reaches
  // Schema() after the resume, next to inner_ and gate_.
  std::vector<std::string> phases;
  ToolInvocationContext context;
  context.event = [&phases](std::string_view phase, const ToolInvocationContext&,
                            std::string_view, std::string_view) {
    phases.emplace_back(phase);
  };
  InvocationToolView view(registry, {}, {}, context, nullptr);
  ASSERT_TRUE(view.AddFromRegistry(registry, "write", ToolTier::kReadonly).ok());

  auto future = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await view.Invoke("write", "{}", "call-1", CancelToken{});
      },
      asio::use_future);
  // Start the call, then stop as soon as it is parked on the timer.
  while (first->started == 0) {
    io.poll();
    io.restart();
  }

  // A skill load re-registers "write" while the call is suspended. The wrapper
  // the call started in must survive: GatedTool::Invoke holds a raw `this` and
  // reaches back into inner_/Schema() after the resume.
  auto second = std::make_shared<SuspendingTool>(release, "second");
  registry.Register(second);
  ASSERT_TRUE(view.AddFromRegistry(registry, "write", ToolTier::kReadonly).ok());

  release->cancel();
  io.run();
  EXPECT_EQ(future.get(), "first");
  EXPECT_EQ(second->started, 0);
  EXPECT_EQ(phases, (std::vector<std::string>{"requested", "completed"}));
}

TEST(InvocationToolViewTest, UnchangedReRegistrationIsANoOp) {
  int invoked = 0;
  ToolRegistry registry;
  registry.Register(NativeCounter(&invoked));
  auto gate = std::make_shared<FixedGate>(ToolGateDecision{true, {}, {}});
  InvocationToolView view(registry, {"write"}, {}, {}, gate);
  ASSERT_TRUE(view.Has("write"));

  // Same tool at the same tier keeps the wrapper the constructor built; a
  // different tier is a real update and re-wraps.
  ASSERT_TRUE(view.AddFromRegistry(registry, "write", ToolTier::kReadonly).ok());
  asio::io_context io;
  EXPECT_EQ(InvokeView(view, "write", io), "ran");
  EXPECT_EQ(gate->calls, 0);

  ASSERT_TRUE(view.AddFromRegistry(registry, "write", ToolTier::kConfirm).ok());
  EXPECT_EQ(InvokeView(view, "write", io), "ran");
  EXPECT_EQ(gate->calls, 1);
  EXPECT_EQ(invoked, 2);
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
