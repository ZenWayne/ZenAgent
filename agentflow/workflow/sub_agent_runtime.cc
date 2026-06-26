#include "agentflow/workflow/sub_agent_runtime.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/inference/litert_lm_conversation.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/workflow/json_path.h"
#include "agentflow/workflow/template_engine.h"
#include "workflow_spec.pb.h"

namespace agentflow::workflow {
namespace {

std::string GenUuidLike() {
  std::random_device rd;
  std::mt19937_64 rng(rd());
  std::ostringstream s;
  s << std::hex << rng() << "-" << rng();
  return s.str();
}

const proto::WorkflowSpec::AgentDef* FindAgent(const Workflow& wf,
                                                  std::string_view name) {
  auto it = wf.spec().agents().find(std::string(name));
  if (it == wf.spec().agents().end()) return nullptr;
  return &it->second;
}

// Pulls the text out of an assistant message-JSON envelope:
//   {"role":"assistant","content":[{"type":"text","text":"..."}, ...]}
std::string ExtractTextDelta(const nlohmann::ordered_json& msg) {
  if (!msg.contains("content")) return {};
  const auto& content = msg["content"];
  if (!content.is_array()) return {};
  std::string out;
  for (const auto& item : content) {
    if (item.value("type", "") == "text" && item.contains("text") &&
        item["text"].is_string()) {
      out += item["text"].get<std::string>();
    }
  }
  return out;
}

}  // namespace

SubAgentRuntime::SubAgentRuntime(
    std::shared_ptr<Workflow> wf, const ToolRegistry& host_tools,
    EventEmitter& emit, ConversationFactory conv_factory)
    : wf_(std::move(wf)),
      host_tools_(host_tools),
      emit_(emit),
      conv_factory_(std::move(conv_factory)) {}

SubAgentRuntime::ConversationFactory
SubAgentRuntime::DefaultConversationFactory(
    std::shared_ptr<::agentflow::LiteRtLmEngine> engine,
    ::asio::io_context& io) {
  return [engine = std::move(engine), &io](
             LiteRtLmConversationOptions opts) -> SendFn {
    const bool constrained = opts.constrained_tool_calls;
    auto conv = LiteRtLmConversation::Create(engine, std::move(opts), io);
    if (!conv) return SendFn{};
    // The conversation owns history server-side; capture it so successive
    // SendFn calls form a multi-turn exchange.
    return [conv, constrained,
            registered = std::make_shared<std::atomic_bool>(false)](
               const std::string& message_json, const TokenSink& on_token,
               const ::agentflow::CancelToken& cancel)
               -> asio::awaitable<absl::StatusOr<std::string>> {
      // Register the in-flight cancel hook once: a cancel breaks the engine
      // request (streaming or sync) mid-decode, not just at turn boundaries.
      if (!registered->exchange(true)) {
        cancel.OnCancel([conv]() { conv->Cancel(); });
      }
      // Non-streaming path: constrained conversations have no streaming C
      // bridge, and a missing sink means nobody wants deltas. SendMessageSync
      // blocks the io thread for the turn (same as the constrained AgentNode
      // path).
      if (constrained || !on_token) {
        co_return conv->SendMessageSync(message_json);
      }
      // Streaming path: drive the async stream, forward each text delta, and
      // rebuild ONE canonical response JSON (each chunk is a full message-JSON
      // envelope; raw concatenation would be invalid). Mirrors AgentNode.
      conv->SendMessage(message_json);
      std::string acc_text;
      nlohmann::ordered_json tool_call_msg;
      bool saw_tool_calls = false;
      for (;;) {
        std::string chunk;
        try {
          chunk = co_await conv->NextTokenAsync();
        } catch (const std::exception&) {
          co_return absl::InternalError("sub-agent streaming send failed");
        }
        if (chunk.empty()) break;  // end of turn
        auto cj = nlohmann::ordered_json::parse(chunk, nullptr, false);
        if (cj.is_discarded()) {
          on_token(chunk);
          acc_text += chunk;
          continue;
        }
        if (cj.contains("tool_calls") && cj["tool_calls"].is_array() &&
            !cj["tool_calls"].empty()) {
          saw_tool_calls = true;
          tool_call_msg = std::move(cj);
          continue;
        }
        std::string delta = ExtractTextDelta(cj);
        if (!delta.empty()) {
          on_token(delta);
          acc_text += delta;
        }
      }
      if (saw_tool_calls) {
        co_return tool_call_msg.dump();
      }
      nlohmann::ordered_json msg = {
          {"role", "assistant"},
          {"content", nlohmann::ordered_json::array(
                          {{{"type", "text"}, {"text", acc_text}}})}};
      co_return msg.dump();
    };
  };
}

asio::awaitable<nlohmann::ordered_json> SubAgentRuntime::RunAsync(
    std::string_view parent_agent, std::string_view child_agent,
    std::string_view goal, SubAgentContext ctx) {
  std::string invocation_id = GenUuidLike();
  std::string root_id =
      ctx.depth == 0 ? invocation_id : ctx.root_invocation_id;

  const auto* parent_def = FindAgent(*wf_, parent_agent);
  if (!parent_def || !parent_def->has_delegates()) {
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "unknown_agent", 0);
    co_return nlohmann::ordered_json{{"error", "unknown_agent"}};
  }
  if (ctx.depth >= parent_def->delegates().max_depth()) {
    emit_.EmitSubAgentStart(parent_agent, child_agent, invocation_id, root_id,
                              ctx.depth, goal);
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false,
                           "max_depth_exceeded", 0);
    co_return nlohmann::ordered_json{
        {"error", "max_depth_exceeded"},
        {"depth", static_cast<uint32_t>(ctx.depth)}};
  }
  // Child must be in roster.
  bool in_roster = false;
  for (const auto& a : parent_def->delegates().agents()) {
    if (a == child_agent) {
      in_roster = true;
      break;
    }
  }
  if (!in_roster) {
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "unknown_agent", 0);
    co_return nlohmann::ordered_json{{"error", "unknown_agent"}};
  }

  emit_.EmitSubAgentStart(parent_agent, child_agent, invocation_id, root_id,
                            ctx.depth, goal);

  const auto& child = wf_->spec().agents().at(std::string(child_agent));

  // Build system message + per-child tool slice + send the goal as the
  // initial user message, then run the multi-turn tool dispatch loop. The
  // conversation is obtained through the injected factory so tests can drive
  // this path with a fake; there is no test-only branch here.
  EvalContext sys_ctx;
  sys_ctx.workflow_name = wf_->name();
  sys_ctx.workflow_version = wf_->version();
  std::string system_text;
  if (!child.system_prompt().empty()) {
    auto tmpl_or = TemplateString::Parse(child.system_prompt());
    if (!tmpl_or.ok()) {
      emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "bad_template",
                              0);
      co_return nlohmann::ordered_json{{"error", "bad_template"}};
    }
    auto v = tmpl_or->Evaluate(sys_ctx);
    system_text = v.is_string() ? v.get<std::string>() : v.dump();
  }

  // Per-child tool slicing (Task 4.6).
  std::vector<std::string> child_tools;
  child_tools.reserve(child.tools_size());
  for (const auto& t : child.tools()) child_tools.push_back(t);
  std::string tools_json = host_tools_.ExportToolsJson(
      std::span<const std::string>(child_tools));
  if (tools_json.empty()) tools_json = "[]";

  // Build the LiteRtLmConversation. system_message_json must be the full
  // {"role":"system","content":"..."} JSON object — that's what the engine
  // feeds the chat template.
  LiteRtLmConversationOptions opts;
  if (!system_text.empty()) {
    nlohmann::ordered_json sys_msg = {{"role", "system"},
                                        {"content", system_text}};
    opts.system_message_json = sys_msg.dump();
  }
  opts.tools_json = tools_json;
  opts.constrained_tool_calls = child.model().constrained_tool_calls();
  if (child.model().max_output_tokens() > 0) {
    opts.max_output_tokens = child.model().max_output_tokens();
  } else {
    opts.max_output_tokens = 512;
  }

  SendFn send = conv_factory_ ? conv_factory_(std::move(opts)) : SendFn{};
  if (!send) {
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "engine_error", 0);
    co_return nlohmann::ordered_json{{"error", "engine_error"}};
  }

  // Per-invocation token forwarding: hand each streamed delta to the run-wide
  // sink (wired by the delegate tool). Unset → no-op (non-streaming or nobody
  // listening). The conversation also won't stream when the child is
  // constrained (handled inside the SendFn). ctx.token_sink is already the
  // right type, so no adapter is needed.
  const TokenSink& on_token = ctx.token_sink;

  nlohmann::ordered_json user_msg = {
      {"role", "user"},
      {"content", nlohmann::ordered_json::array(
                       {{{"type", "text"}, {"text", std::string(goal)}}})}};

  // Multi-turn tool dispatch. The sub-agent's tool slice is host_tools_ +
  // child.tools[]. The LiteRtLmConversation carries history server-side.
  std::string message_json = user_msg.dump();
  std::string raw_response;
  constexpr int kSubAgentMaxIter = 8;
  // CancelToken sourced once for the whole loop — used when parent_cancel
  // is null.
  ::agentflow::CancelSource local_src;
  ::agentflow::CancelToken local_tok = local_src.Token();
  const ::agentflow::CancelToken& cancel_ref =
      ctx.parent_cancel ? *ctx.parent_cancel : local_tok;
  for (int iter = 0; iter < kSubAgentMaxIter; ++iter) {
    if (ctx.parent_cancel && ctx.parent_cancel->IsCancelled()) {
      emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "cancelled", 0);
      co_return nlohmann::ordered_json{{"error", "cancelled"}};
    }
    auto resp_or = co_await send(message_json, on_token, cancel_ref);
    if (!resp_or.ok()) {
      emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "engine_error", 0);
      co_return nlohmann::ordered_json{{"error", "engine_error"}};
    }
    raw_response = *resp_or;

    auto resp_json =
        nlohmann::ordered_json::parse(raw_response, nullptr, false);
    if (resp_json.is_discarded()) break;  // raw text — no further dispatch

    if (resp_json.contains("tool_calls") &&
        resp_json["tool_calls"].is_array() &&
        !resp_json["tool_calls"].empty()) {
      nlohmann::ordered_json tool_content =
          nlohmann::ordered_json::array();
      for (const auto& tc : resp_json["tool_calls"]) {
        std::string name = tc.value(
            "name", tc.value("function", nlohmann::ordered_json::object())
                         .value("name", ""));
        std::string args;
        if (tc.contains("arguments")) {
          args = tc["arguments"].is_string()
                     ? tc["arguments"].get<std::string>()
                     : tc["arguments"].dump();
        } else if (tc.contains("function") &&
                   tc["function"].contains("arguments")) {
          args = tc["function"]["arguments"].is_string()
                     ? tc["function"]["arguments"].get<std::string>()
                     : tc["function"]["arguments"].dump();
        }
        // Sub-agents dispatch tools through the host registry. The host
        // is responsible for restricting which tools exist; the per-child
        // tools_json slice scopes what the LLM sees but the dispatcher
        // can technically call any host tool. We rely on the LLM
        // respecting the schema; defense-in-depth filtering is a future
        // improvement.
        std::string result;
        // ToolRegistry::Invoke is not const (its mutex is mutable but the
        // method signature isn't); we hold a const& as a member. Cast away
        // const for the dispatch — safe given the registry's internal
        // locking. Direct co_await — RunAsync runs under the caller's
        // io_context, so no nested io.run() (that would deadlock a concurrent
        // token-channel drain).
        auto& reg = const_cast<ToolRegistry&>(host_tools_);
        try {
          result = co_await reg.Invoke(name, args, cancel_ref);
        } catch (const std::exception& e) {
          result = std::string("Tool error: ") + e.what();
        }
        tool_content.push_back(
            {{"name", name}, {"response", {{"value", result}}}});
      }
      nlohmann::ordered_json tool_msg = {{"role", "tool"},
                                           {"content", tool_content}};
      message_json = tool_msg.dump();
      continue;
    }
    break;  // No tool calls — done.
  }

  // Output extraction.
  std::string extract = parent_def->delegates().output_extract().empty()
                            ? "$.content[0].text"
                            : parent_def->delegates().output_extract();
  auto path_or = JsonPath::Parse(extract);
  std::string final_str;
  if (path_or.ok()) {
    auto root = nlohmann::ordered_json::parse(raw_response, nullptr, false);
    if (!root.is_discarded()) {
      auto val = path_or->Resolve(root);
      if (val && val->is_string()) {
        final_str = val->get<std::string>();
      } else {
        emit_.EmitSubAgentExtractFailed(invocation_id, extract);
        final_str = raw_response;
      }
    } else {
      final_str = raw_response;
    }
  } else {
    final_str = raw_response;
  }

  emit_.EmitSubAgentEnd(invocation_id, ctx.depth, true, "",
                          static_cast<uint32_t>(final_str.size()));
  co_return nlohmann::ordered_json(final_str);
}

}  // namespace agentflow::workflow
