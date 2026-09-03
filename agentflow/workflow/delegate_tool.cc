#include "agentflow/workflow/delegate_tool.h"

#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>

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
    ::asio::io_context* io,
    TokenChannel* top_channel) {
  ::agentflow::ToolSchema schema{
      "delegate",
      "Hand a sub-task to another agent. The chosen agent runs with clean "
      "context and returns a result string.",
      BuildSchema(allowed_children)};
  auto fn = [runtime, parent_agent = std::move(parent_agent), ctx, io,
             top_channel](std::string_view args_json,
                          std::string_view /*tool_call_id*/,
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

    SubAgentContext sub_ctx = ctx;

    // Per-invocation token stream: one fresh channel per delegate call (so
    // concurrent/sequential sub-agents never share a channel). A drain
    // coroutine forwards each delta up to the run-wide top_channel; an
    // empty-string sentinel (sent after the sub-agent finishes) ends it
    // cleanly so no buffered token is dropped.
    std::shared_ptr<TokenChannel> sub_ch;
    if (io != nullptr && top_channel != nullptr) {
      sub_ch = std::make_shared<TokenChannel>(*io, /*capacity=*/4096);
      sub_ctx.token_channel = sub_ch.get();
      TokenChannel* top = top_channel;
      asio::co_spawn(*io, [sub_ch, top]() -> asio::awaitable<void> {
        for (;;) {
          auto [rec_ec, tok] = co_await sub_ch->async_receive(
              asio::as_tuple(asio::use_awaitable));
          if (rec_ec || tok.empty()) break;  // closed or end-of-stream sentinel
          auto [snd_ec] = co_await top->async_send(
              asio::error_code{}, std::move(tok),
              asio::as_tuple(asio::use_awaitable));
          if (snd_ec) break;  // top consumer gone
        }
        co_return;
      }, asio::detached);
    }

    auto result =
        co_await runtime->RunAsync(parent_agent, agent, goal, sub_ctx);

    if (sub_ch) {
      // End-of-stream sentinel — drains after all real deltas (FIFO).
      auto [ec] = co_await sub_ch->async_send(
          asio::error_code{}, std::string{},
          asio::as_tuple(asio::use_awaitable));
      (void)ec;
    }

    if (result.is_string()) co_return result.get<std::string>();
    co_return result.dump();
  };
  return std::make_shared<::agentflow::NativeFnTool>(schema, std::move(fn));
}

}  // namespace agentflow::workflow
