#include "agentflow/workflow/sub_agent_runtime.h"

#include <cstdint>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/use_awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/errors.h"
#include "agentflow/inference/chat_backend.h"
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

}  // namespace

std::shared_ptr<::agentflow::IChatBackend> ResolveNamedBackend(
    std::string_view backend_name, std::string_view requesting_agent,
    const std::shared_ptr<::agentflow::IChatBackend>& default_backend,
    const std::map<std::string, std::shared_ptr<::agentflow::IChatBackend>>&
        backends) {
  if (backend_name.empty()) return default_backend;
  auto it = backends.find(std::string(backend_name));
  if (it == backends.end()) {
    throw AgentflowError("agent '" + std::string(requesting_agent) +
                          "' requests backend '" + std::string(backend_name) +
                          "' which the host did not register");
  }
  return it->second;
}

SubAgentRuntime::SubAgentRuntime(
    std::shared_ptr<Workflow> wf, const ToolRegistry& host_tools,
    EventEmitter& emit, ConversationFactory conv_factory)
    : wf_(std::move(wf)),
      host_tools_(host_tools),
      emit_(emit),
      conv_factory_(std::move(conv_factory)) {}

SubAgentRuntime::ConversationFactory
SubAgentRuntime::DefaultConversationFactory(
    std::shared_ptr<::agentflow::IChatBackend> default_backend,
    std::map<std::string, std::shared_ptr<::agentflow::IChatBackend>>
        backends) {
  return [default_backend = std::move(default_backend),
          backends = std::move(backends)](
             std::string_view backend_name, std::string_view requesting_agent,
             ::agentflow::ChatConversationOptions opts) -> SendFn {
    auto backend = ResolveNamedBackend(backend_name, requesting_agent,
                                        default_backend, backends);
    auto conv = backend->CreateConversation(std::move(opts));
    if (!conv) return SendFn{};
    return [conv](const std::string& message_json, const TokenSink& on_token,
                   const ::agentflow::CancelToken& cancel)
               -> asio::awaitable<absl::StatusOr<std::string>> {
      co_return co_await conv->SendAsync(message_json, on_token, cancel);
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

  // Build the conversation options. system_message_json must be the BARE
  // content array documented on ChatConversationOptions
  // ([{"type":"text","text":"..."}]), NOT a {role,content} object — this
  // mirrors AgentNode::BuildSystemMessageJson exactly. LiteRT-LM wraps it
  // into {role:system, content:<this>} itself; a remote OpenAI-compatible
  // backend's openai::SystemMessage() only recognises this bare-array shape
  // too (FlattenContent returns {} for a {role,content} object, so a
  // {role,content} object here silently drops the system prompt on any
  // remote backend).
  ::agentflow::ChatConversationOptions opts;
  if (!system_text.empty()) {
    nlohmann::ordered_json sys_msg = nlohmann::ordered_json::array(
        {{{"type", "text"}, {"text", system_text}}});
    opts.system_message_json = sys_msg.dump();
  }
  opts.tools_json = tools_json;
  opts.constrained_tool_calls = child.model().constrained_tool_calls();
  if (child.model().max_output_tokens() > 0) {
    opts.max_output_tokens = child.model().max_output_tokens();
  } else {
    opts.max_output_tokens = 512;
  }

  // Resolve the child's OWN backend (Task fix: sub-agents used to silently
  // run on the parent's resolved backend even when they declared their own
  // model.backend). An empty name falls back to the host default inside
  // ResolveNamedBackend; an unregistered name throws — same rule as
  // top-level agent backend selection (workflow_runner.cc), enforced by
  // DefaultConversationFactory when conv_factory_ is the production one.
  // ResolveNamedBackend (reached via conv_factory_ when conv_factory_ is
  // DefaultConversationFactory) THROWS AgentflowError on an unregistered
  // backend name rather than silently falling back to the default — see its
  // doc comment. RunAsync's own contract is "NEVER throws", so that throw is
  // caught here and converted into the same structured-error shape every
  // other failure in this function uses: EmitSubAgentEnd terminates the
  // trace span, and {"error":"unknown_backend",...} reaches the parent LLM
  // as loudly as engine_error does — it must NOT be swallowed into a silent
  // fallback to the parent's or host's default backend (design spec §5).
  SendFn send;
  try {
    send = conv_factory_
               ? conv_factory_(child.model().backend(), child_agent,
                                std::move(opts))
               : SendFn{};
  } catch (const AgentflowError&) {
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "unknown_backend",
                           0);
    co_return nlohmann::ordered_json{
        {"error", "unknown_backend"},
        {"backend", std::string(child.model().backend())}};
  }
  if (!send) {
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "engine_error", 0);
    co_return nlohmann::ordered_json{{"error", "engine_error"}};
  }

  // Per-invocation token forwarding: push each streamed delta onto the
  // caller-provided channel (set by the delegate tool, one per call). Unset →
  // no-op (non-streaming or nobody listening). The conversation also won't
  // stream when the child is constrained (handled inside the SendFn).
  TokenSink on_token;
  if (ctx.token_channel != nullptr) {
    auto* ch = ctx.token_channel;
    on_token = [ch](std::string_view delta) -> asio::awaitable<void> {
      // Back-pressured: a slow consumer slows the sub-agent's decode loop
      // rather than losing tokens. as_tuple so a closed channel yields an
      // error instead of throwing.
      auto [ec] = co_await ch->async_send(asio::error_code{},
                                           std::string(delta),
                                           asio::as_tuple(asio::use_awaitable));
      (void)ec;
      co_return;
    };
  }

  nlohmann::ordered_json user_msg = {
      {"role", "user"},
      {"content", nlohmann::ordered_json::array(
                       {{{"type", "text"}, {"text", std::string(goal)}}})}};

  // Multi-turn tool dispatch. The sub-agent's tool slice is host_tools_ +
  // child.tools[]. The backend's Conversation carries history server-side.
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
        // json::value() THROWS type_error.306 on a non-object and
        // type_error.302 when a present key has the wrong type. Model
        // output is untrusted — never read a field without proving BOTH
        // that its container is an object AND that the field has the type
        // we are about to read it as. A malformed entry degrades to a
        // SKIPPED entry, never an uncaught exception. Same shape as
        // StreamAccumulator::Feed / AgentNode::Run's twin guard.
        if (!tc.is_object()) continue;
        std::string name;
        if (tc.contains("name") && tc["name"].is_string()) {
          name = tc["name"].get<std::string>();
        } else if (tc.contains("function") && tc["function"].is_object()) {
          const auto& fn = tc["function"];
          if (fn.contains("name") && fn["name"].is_string()) {
            name = fn["name"].get<std::string>();
          }
        }
        // The originating call's id. LiteRT-LM does not need it (the Gemma
        // template reads only name/response), but OpenAI-compatible backends
        // must echo it back as tool_call_id — message_map.cc's
        // ToOpenAiMessages rejects a tool-result entry with no id. Same
        // extraction as AgentNode::Run's twin loop (design spec §3.2).
        std::string call_id;
        if (tc.contains("id") && tc["id"].is_string()) {
          call_id = tc["id"].get<std::string>();
        }
        std::string args;
        if (tc.contains("arguments")) {
          args = tc["arguments"].is_string()
                     ? tc["arguments"].get<std::string>()
                     : tc["arguments"].dump();
        } else if (tc.contains("function") && tc["function"].is_object() &&
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
        nlohmann::ordered_json entry = {{"name", name},
                                          {"response", {{"value", result}}}};
        if (!call_id.empty()) entry["id"] = call_id;
        tool_content.push_back(std::move(entry));
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
