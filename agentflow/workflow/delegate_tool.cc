#include "agentflow/workflow/delegate_tool.h"

#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#include <asio/awaitable.hpp>

#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/workflow/sub_agent_runtime.h"

namespace agentflow::workflow {
namespace {

std::string BuildSchema(const std::vector<std::string>& allowed) {
  nlohmann::ordered_json agent_prop = {{"type", "string"}};
  nlohmann::ordered_json enum_arr = nlohmann::ordered_json::array();
  for (const auto& a : allowed) enum_arr.push_back(a);
  agent_prop["enum"] = enum_arr;
  nlohmann::ordered_json props;
  props["agent"] = agent_prop;
  props["goal"]  = nlohmann::ordered_json{{"type", "string"}};
  nlohmann::ordered_json required = nlohmann::ordered_json::array();
  required.push_back("agent");
  required.push_back("goal");
  nlohmann::ordered_json schema;
  schema["type"] = "object";
  schema["properties"] = props;
  schema["required"] = required;
  return schema.dump();
}

}  // namespace

std::shared_ptr<::agentflow::Tool> MakeDelegateTool(
    std::shared_ptr<SubAgentRuntime> runtime,
    std::string parent_agent,
    std::vector<std::string> allowed_children,
    SubAgentContext ctx,
    TokenSink top_sink) {
  ::agentflow::ToolSchema schema{
      "delegate",
      "Hand a sub-task to another agent. The chosen agent runs with clean "
      "context and returns a result string.",
      BuildSchema(allowed_children)};
  auto fn = [runtime, parent_agent = std::move(parent_agent), ctx,
             top_sink = std::move(top_sink)](
                std::string_view args_json,
                const ::agentflow::CancelToken& cancel)
                -> asio::awaitable<std::string> {
    auto args = nlohmann::ordered_json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
      co_return std::string(R"({"error":"bad_args"})");
    }
    std::string agent = args.value("agent", "");
    std::string goal = args.value("goal", "");
    if (cancel.IsCancelled()) {
      co_return std::string(R"({"error":"cancelled"})");
    }
    if (!runtime) {
      co_return std::string(R"({"error":"no_runtime"})");
    }

    // The sub-agent streams onto the shared run-wide sink directly: a callback
    // is inherently many-to-one, so no per-call channel or drain coroutine is
    // needed for fan-in. Ordering is preserved by the single cooperative io
    // thread (one coroutine runs between suspension points).
    SubAgentContext sub_ctx = ctx;
    sub_ctx.token_sink = top_sink;

    auto result =
        co_await runtime->RunAsync(parent_agent, agent, goal, sub_ctx);

    if (result.is_string()) co_return result.get<std::string>();
    co_return result.dump();
  };
  return std::make_shared<::agentflow::NativeFnTool>(schema, std::move(fn));
}

}  // namespace agentflow::workflow
