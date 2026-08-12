// agentflow/nodes/agent_node.cc
#include "agentflow/nodes/agent_node.h"

#include <nlohmann/json.hpp>

#include <absl/status/status.h>
#include <asio/as_tuple.hpp>
#include <asio/use_awaitable.hpp>

#include "agentflow/core/errors.h"
#include "agentflow/inference/canonical_message.h"

namespace agentflow {

namespace {

using json = nlohmann::json;

}  // namespace

AgentNode::AgentNode(AgentNodeConfig cfg)
    : cfg_(std::move(cfg)),
      id_("agent") {}

std::string AgentNode::BuildSystemMessageJson() const {
  if (cfg_.system_prompt.empty()) return {};
  // LiteRT-LM wraps this into {role:system, content:<this>}. The Gemma
  // jinja template iterates content as a sequence of {type,text} items, so
  // pass an array — a single object falls through both string and sequence
  // branches and gets silently dropped.
  json sys = json::array({{{"type", "text"}, {"text", cfg_.system_prompt}}});
  return sys.dump();
}

std::string AgentNode::BuildToolsJson() const {
  if (!cfg_.tool_registry && cfg_.extra_tools.empty()) return "[]";
  json arr;
  if (cfg_.tool_registry) {
    arr = json::parse(cfg_.tool_registry->ExportToolsJson(cfg_.tool_names));
  } else {
    arr = json::array();
  }
  for (const auto& tool : cfg_.extra_tools) {
    if (!tool) continue;
    const auto& schema = tool->Schema();
    json entry = {{"type", "function"},
                  {"function", {{"name", schema.name},
                                {"description", schema.description},
                                {"parameters",
                                 json::parse(schema.params_json_schema)}}}};
    arr.push_back(std::move(entry));
  }
  return arr.dump();
}

std::string AgentNode::BuildUserMessageJson(const State& state) const {
  std::string user_text = ReadStringField(state, cfg_.input_field);
  json msg = {
    {"role", "user"},
    {"content", json::array({{{"type", "text"}, {"text", user_text}}})},
  };
  return msg.dump();
}

void AgentNode::WriteOutput(State& state, const std::string& text) const {
  WriteStringField(state, cfg_.output_field, text);
}

asio::awaitable<State> AgentNode::Run(
    State state, const CancelToken& cancel, EventEmitter& emit) {
  if (!cfg_.backend || !cfg_.io_ctx) {
    throw AgentflowError("AgentNode: backend and io_ctx must be configured");
  }
  if (cancel.IsCancelled()) co_return std::move(state);

  // One conversation per Run. The backend owns history across turns.
  ChatConversationOptions opts;
  opts.system_message_json = BuildSystemMessageJson();
  opts.tools_json = BuildToolsJson();
  opts.max_output_tokens = cfg_.max_output_tokens;
  opts.constrained_tool_calls = cfg_.constrained_tool_calls;

  auto conv = cfg_.backend->CreateConversation(std::move(opts));
  if (!conv) {
    throw AgentflowError(
        "AgentNode: failed to create conversation on backend " +
        std::string(cfg_.backend->Describe()));
  }

  // Cooperative cancellation: break the in-flight request so a streaming turn
  // stops mid-decode, not only at the next turn boundary. Safe from any
  // thread.
  cancel.OnCancel([conv]() { conv->Cancel(); });

  std::string message_json = BuildUserMessageJson(state);
  std::string final_answer;

  for (int iter = 0; iter < cfg_.max_iter; ++iter) {
    if (cancel.IsCancelled()) break;

    // One path for both streaming and non-streaming: the backend decides
    // whether deltas are available and reports them through the sink.
    //
    // NOTE: a constrained conversation produces no deltas — the backend never
    // invokes this sink while constrained_tool_calls is set, so no token or
    // trace events are emitted for that combination. Deliberate: constrained
    // decoding has no real increments, and emitting the whole response as one
    // "delta" would misrepresent it to a streaming UI.
    TokenSink sink;
    if (cfg_.stream_tokens) {
      sink = [this, &emit](std::string_view delta)
                 -> asio::awaitable<void> {
        emit.EmitToken(Id(), delta);
        if (cfg_.token_channel) {
          // Back-pressured send: a consumer that is not keeping up slows the
          // decode loop rather than losing tokens. as_tuple so a closed
          // channel (consumer gone) yields an error instead of throwing — we
          // simply stop forwarding in that case. This is the same behaviour
          // the pre-refactor AgentNode had; it is preserved exactly.
          auto [ec] = co_await cfg_.token_channel->async_send(
              asio::error_code{}, std::string(delta),
              asio::as_tuple(asio::use_awaitable));
          (void)ec;
        }
        co_return;
      };
    }

    auto resp_or = co_await conv->SendAsync(message_json, sink, cancel);
    if (!resp_or.ok()) {
      if (absl::IsCancelled(resp_or.status())) break;
      throw AgentflowError("AgentNode: backend send failed: " +
                            std::string(resp_or.status().message()));
    }
    std::string resp_str = *resp_or;

    json resp;
    try {
      resp = json::parse(resp_str);
    } catch (const json::exception&) {
      final_answer = resp_str;  // fall back to raw text
      break;
    }

    // Tool dispatch: LiteRT-LM puts tool_calls at the top level of the
    // assistant message when present.
    if (resp.contains("tool_calls") && resp["tool_calls"].is_array() &&
        !resp["tool_calls"].empty()) {
      // Dispatch every tool call this turn, then send a single tool-role
      // message back with each result.
      json tool_content = json::array();
      for (const auto& tc : resp["tool_calls"]) {
        // json::value() THROWS type_error.306 on a non-object, and the
        // try/catch above covers only the outer parse. Model output is
        // untrusted — skip anything that is not an object.
        if (!tc.is_object()) continue;
        std::string name = tc.value("name", tc.value("function",
                                                      json::object())
                                                .value("name", ""));
        // The originating call's id. LiteRT-LM does not need it (the Gemma
        // template reads only name/response), but OpenAI-compatible backends
        // must echo it back as tool_call_id, so it is threaded through the
        // canonical shape. Design spec §3.2.
        std::string call_id = tc.value("id", "");
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
        std::string result = co_await DispatchTool(name, args, cancel, emit);
        // Per the Gemma jinja template each content item carries `name` +
        // `response` directly; `id` is additive and ignored on that path.
        json entry = {
          {"name", name},
          {"response", {{"value", result}}},
        };
        if (!call_id.empty()) entry["id"] = call_id;
        tool_content.push_back(std::move(entry));
      }
      json tool_message = {{"role", "tool"}, {"content", tool_content}};
      message_json = tool_message.dump();
      continue;
    }

    // No tool calls — extract the assistant text and we're done.
    final_answer = ExtractAssistantText(resp_str);
    if (final_answer.empty()) final_answer = resp_str;
    break;
  }

  if (final_answer.empty() && !cancel.IsCancelled()) {
    WriteOutput(state, "Agent reached maximum iterations without a final answer.");
  } else {
    WriteOutput(state, final_answer);
  }

  co_return std::move(state);
}

asio::awaitable<std::string> AgentNode::DispatchTool(
    const std::string& name, const std::string& args,
    const CancelToken& cancel, EventEmitter& emit) {
  // Extras take precedence over the registry on name collisions.
  for (const auto& tool : cfg_.extra_tools) {
    if (!tool) continue;
    if (tool->Schema().name == name) {
      emit.EmitToolCall(Id(), name, args);
      std::string result;
      try {
        result = co_await tool->Invoke(args, cancel);
      } catch (const std::exception& e) {
        result = std::string("Tool error: ") + e.what();
      }
      emit.EmitToolReturn(Id(), name, result);
      co_return result;
    }
  }
  if (!cfg_.tool_registry) co_return std::string{};

  emit.EmitToolCall(Id(), name, args);
  std::string result;
  try {
    result = co_await cfg_.tool_registry->Invoke(name, args, cancel);
  } catch (const std::exception& e) {
    result = std::string("Tool error: ") + e.what();
  }
  emit.EmitToolReturn(Id(), name, result);
  co_return result;
}

}  // namespace agentflow
