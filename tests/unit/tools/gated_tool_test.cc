#include "agentflow/tools/gated_tool.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/invocation_tool_view.h"
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

std::string InvokeBlocking(Tool& tool, asio::io_context& io, std::string_view args,
                const CancelToken& cancel = CancelToken{}) {
  auto future = asio::co_spawn(io, [&]() -> asio::awaitable<std::string> {
    co_return co_await tool.Invoke(args, "call-1", cancel);
  }, asio::use_future);
  io.run();
  return future.get();
}

TEST(GatedToolTest, RejectionNeverInvokesInnerTool) {
  int invoked = 0;
  auto inner = std::make_shared<NativeFnTool>(ToolSchema{"danger", "", "{}"},
      [&invoked](auto, auto, auto) -> asio::awaitable<std::string> {
        ++invoked; co_return "unexpected";
      });
  auto gate = std::make_shared<FixedGate>(ToolGateDecision{false, "rejected_by_user", {}});
  GatedTool tool(inner, ToolTier::kConfirm, {}, gate);
  asio::io_context io;
  EXPECT_EQ(InvokeBlocking(tool, io, R"({"x":1})"), R"({"error":"rejected_by_user"})");
  EXPECT_EQ(invoked, 0);
  EXPECT_EQ(gate->calls, 1);
}

TEST(GatedToolTest, ApprovalEditsOnlyThisInvocation) {
  std::string seen;
  auto inner = std::make_shared<NativeFnTool>(ToolSchema{"write", "", "{}"},
      [&seen](std::string_view args, auto, auto) -> asio::awaitable<std::string> {
        seen = args; co_return "ok";
      });
  auto gate = std::make_shared<FixedGate>(ToolGateDecision{true, {}, R"({"safe":true})"});
  GatedTool tool(inner, ToolTier::kConfirm, {}, gate);
  asio::io_context io;
  EXPECT_EQ(InvokeBlocking(tool, io, R"({"unsafe":true})"), "ok");
  EXPECT_EQ(seen, R"({"safe":true})");
}

TEST(GatedToolTest, BlockedAndCancelledDoNotInvokeInnerTool) {
  int invoked = 0;
  auto inner = std::make_shared<NativeFnTool>(ToolSchema{"x", "", "{}"},
      [&invoked](auto, auto, auto) -> asio::awaitable<std::string> { ++invoked; co_return "ok"; });
  GatedTool blocked(inner, ToolTier::kBlocked, {}, nullptr);
  asio::io_context io;
  EXPECT_EQ(InvokeBlocking(blocked, io, "{}"), R"({"error":"tool_not_allowed"})");
  EXPECT_EQ(invoked, 0);
  io.restart();
  GatedTool readonly(inner, ToolTier::kReadonly, {}, nullptr);
  CancelSource source;
  source.Cancel();
  EXPECT_EQ(InvokeBlocking(readonly, io, "{}", source.Token()), R"({"error":"cancelled"})");
  EXPECT_EQ(invoked, 0);
}

TEST(GatedToolTest, EmitsOneTerminalEventAndPreservesCallContext) {
  std::vector<std::string> phases;
  ToolInvocationContext context;
  context.root_invocation_id = "root";
  context.caller = "child";
  context.delegate_depth = 2;
  context.event = [&phases](std::string_view phase,
                            const ToolInvocationContext& ctx,
                            std::string_view, std::string_view call_id) {
    EXPECT_EQ(ctx.root_invocation_id, "root");
    EXPECT_EQ(ctx.caller, "child");
    EXPECT_EQ(ctx.delegate_depth, 2u);
    EXPECT_EQ(call_id, "call-1");
    phases.emplace_back(phase);
  };
  auto inner = std::make_shared<NativeFnTool>(ToolSchema{"x", "", "{}"},
      [](auto, auto, auto) -> asio::awaitable<std::string> { co_return "ok"; });
  GatedTool tool(inner, ToolTier::kReadonly, std::move(context), nullptr);
  asio::io_context io;
  EXPECT_EQ(InvokeBlocking(tool, io, "{}"), "ok");
  ASSERT_EQ(phases.size(), 2u);
  EXPECT_EQ(phases[0], "requested");
  EXPECT_EQ(phases[1], "completed");
}

TEST(GatedToolTest, InvocationViewHidesAndHardBlocksUndeclaredTools) {
  ToolRegistry registry;
  int safe_calls = 0;
  int hidden_calls = 0;
  registry.Register(std::make_shared<NativeFnTool>(ToolSchema{"safe", "", "{}"},
      [&safe_calls](auto, auto, auto) -> asio::awaitable<std::string> {
        ++safe_calls; co_return "safe"; }));
  registry.Register(std::make_shared<NativeFnTool>(ToolSchema{"hidden", "", "{}"},
      [&hidden_calls](auto, auto, auto) -> asio::awaitable<std::string> {
        ++hidden_calls; co_return "hidden"; }));
  InvocationToolView view(registry, {"safe", "hidden"},
                          {{"hidden", ToolTier::kBlocked}}, {}, nullptr);
  EXPECT_NE(view.ExportToolsJson().find("safe"), std::string::npos);
  EXPECT_EQ(view.ExportToolsJson().find("hidden"), std::string::npos);
  asio::io_context io;
  auto future = asio::co_spawn(io, [&]() -> asio::awaitable<std::string> {
    co_return co_await view.Invoke("hidden", "{}", "call-1", CancelToken{});
  }, asio::use_future);
  io.run();
  EXPECT_EQ(future.get(), R"({"error":"tool_not_allowed"})");
  EXPECT_EQ(safe_calls, 0);
  EXPECT_EQ(hidden_calls, 0);
}

}  // namespace
}  // namespace agentflow
