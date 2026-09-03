// agentflow/tools/native_fn_tool.h
#ifndef AGENTFLOW_TOOLS_NATIVE_FN_TOOL_H_
#define AGENTFLOW_TOOLS_NATIVE_FN_TOOL_H_

#include <functional>
#include <string>
#include <string_view>

#include "agentflow/tools/tool.h"

namespace agentflow {

class NativeFnTool : public Tool {
 public:
  using Fn = std::function<asio::awaitable<std::string>(
      std::string_view, std::string_view, const CancelToken&)>;

  NativeFnTool(ToolSchema schema, Fn fn);
  const ToolSchema& Schema() const override { return schema_; }
  asio::awaitable<std::string> Invoke(
      std::string_view args_json,
      std::string_view tool_call_id,
      const CancelToken& cancel) override;

 private:
  ToolSchema schema_;
  Fn fn_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_TOOLS_NATIVE_FN_TOOL_H_
