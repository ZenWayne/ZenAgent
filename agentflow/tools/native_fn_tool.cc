// agentflow/tools/native_fn_tool.cc
#include "agentflow/tools/native_fn_tool.h"

namespace agentflow {

NativeFnTool::NativeFnTool(ToolSchema schema, Fn fn)
    : schema_(std::move(schema)), fn_(std::move(fn)) {}

asio::awaitable<std::string> NativeFnTool::Invoke(
    std::string_view args_json, const CancelToken& cancel) {
  co_return co_await fn_(args_json, cancel);
}

}  // namespace agentflow
