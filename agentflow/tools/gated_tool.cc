#include "agentflow/tools/gated_tool.h"

#include <exception>
#include <utility>

namespace agentflow {

GatedTool::GatedTool(std::shared_ptr<Tool> inner, ToolTier tier,
                     ToolInvocationContext context,
                     std::shared_ptr<ToolInvocationGate> gate)
    : inner_(std::move(inner)), tier_(tier), context_(std::move(context)), gate_(std::move(gate)) {}

const ToolSchema& GatedTool::Schema() const { return inner_->Schema(); }

void GatedTool::Emit(std::string_view phase, std::string_view call_id) const {
  if (context_.event) context_.event(phase, context_, Schema().name, call_id);
}

asio::awaitable<std::string> GatedTool::Invoke(
    std::string_view args_json, std::string_view tool_call_id,
    const CancelToken& cancel) {
  ToolInvocationContext context = context_;
  if (context.root_invocation_id.empty()) context.root_invocation_id = std::string(tool_call_id);
  auto emit = [&context, this, tool_call_id](std::string_view phase) {
    if (context.event) context.event(phase, context, Schema().name, tool_call_id);
  };
  emit("requested");
  if (tier_ == ToolTier::kBlocked) {
    emit("blocked");
    co_return R"({"error":"tool_not_allowed"})";
  }
  if (cancel.IsCancelled()) {
    emit("cancelled");
    co_return R"({"error":"cancelled"})";
  }
  std::string approved_args(args_json);
  if (tier_ == ToolTier::kConfirm && gate_) {
    ToolGateDecision decision;
    try {
      decision = co_await gate_->Decide(
          Schema().name, args_json, context, tool_call_id, cancel);
    } catch (...) {
      emit("failed");
      throw;
    }
    if (cancel.IsCancelled()) {
      emit("cancelled");
      co_return R"({"error":"cancelled"})";
    }
    if (!decision.approved) {
      emit("rejected");
      const std::string reason = decision.reason.empty() ? "rejected_by_user" : decision.reason;
      co_return std::string("{\"error\":\"") + reason + "\"}";
    }
    if (!decision.approved_args_json.empty()) approved_args = std::move(decision.approved_args_json);
  }
  try {
    std::string result = co_await inner_->Invoke(approved_args, tool_call_id, cancel);
    emit("completed");
    co_return result;
  } catch (...) {
    emit("failed");
    throw;
  }
}

}  // namespace agentflow
