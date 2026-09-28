#ifndef AGENTFLOW_TOOLS_GATED_TOOL_H_
#define AGENTFLOW_TOOLS_GATED_TOOL_H_

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "agentflow/tools/tool.h"

namespace agentflow {

enum class ToolTier { kReadonly, kConfirm, kBlocked };

struct ToolInvocationContext {
  std::string root_invocation_id;
  std::string caller;
  uint32_t delegate_depth = 0;
  // Server-issued callback capability for asynchronous MCP work.  This never
  // enters the model schema or arguments JSON.
  std::string session_id;
  // Lifecycle callbacks deliberately contain no result payload.
  std::function<void(std::string_view phase, const ToolInvocationContext&,
                     std::string_view tool_name, std::string_view call_id)> event;
};

struct ToolGateDecision {
  bool approved = false;
  std::string reason;             // e.g. rejected_by_user / confirmation_timeout
  std::string approved_args_json; // empty means retain the submitted args
};

class ToolInvocationGate {
 public:
  virtual ~ToolInvocationGate() = default;
  virtual asio::awaitable<ToolGateDecision> Decide(
      std::string_view tool_name, std::string_view args_json,
      const ToolInvocationContext& context, std::string_view call_id,
      const CancelToken& cancel) = 0;
};

// A short-lived decorator. It never mutates the shared ToolRegistry or its
// underlying Tool, so confirmation state cannot leak across sessions.
class GatedTool final : public Tool {
 public:
  GatedTool(std::shared_ptr<Tool> inner, ToolTier tier,
            ToolInvocationContext context,
            std::shared_ptr<ToolInvocationGate> gate);

  const ToolSchema& Schema() const override;
  asio::awaitable<std::string> Invoke(std::string_view args_json,
                                      std::string_view tool_call_id,
                                      const CancelToken& cancel) override;

 private:
  void Emit(std::string_view phase, std::string_view call_id) const;
  std::shared_ptr<Tool> inner_;
  ToolTier tier_;
  ToolInvocationContext context_;
  std::shared_ptr<ToolInvocationGate> gate_;
};

}  // namespace agentflow
#endif
