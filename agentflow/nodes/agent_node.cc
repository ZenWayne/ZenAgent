// agentflow/nodes/agent_node.cc
#include "agentflow/nodes/agent_node.h"

#include <nlohmann/json.hpp>

#include "agentflow/core/errors.h"

namespace agentflow {

namespace {

using json = nlohmann::json;

// Pulls the assistant's text content out of the LiteRT-LM response shape:
//   {"role":"assistant","content":[{"type":"text","text":"..."}, ...]}
std::string ExtractAssistantText(const json& resp) {
  if (!resp.contains("content")) return {};
  const auto& content = resp["content"];
  if (!content.is_array()) return {};
  std::string out;
  for (const auto& item : content) {
    if (item.value("type", "") == "text" && item.contains("text") &&
        item["text"].is_string()) {
      out.append(item["text"].get<std::string>());
    }
  }
  return out;
}

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
  if (!cfg_.engine || !cfg_.io_ctx) {
    throw AgentflowError("AgentNode: engine and io_ctx must be configured");
  }
  if (cancel.IsCancelled()) co_return std::move(state);

  // One Conversation per Run. The engine owns history across turns within
  // the ReAct loop; we don't need to thread message state ourselves.
  LiteRtLmConversationOptions opts;
  opts.system_message_json = BuildSystemMessageJson();
  opts.tools_json = BuildToolsJson();
  opts.max_output_tokens = cfg_.max_output_tokens;
  opts.constrained_tool_calls = cfg_.constrained_tool_calls;

  auto conv = LiteRtLmConversation::Create(cfg_.engine, std::move(opts),
                                            *cfg_.io_ctx);
  if (!conv) {
    throw AgentflowError("AgentNode: failed to create Conversation");
  }

  std::string message_json = BuildUserMessageJson(state);
  std::string final_answer;

  for (int iter = 0; iter < cfg_.max_iter; ++iter) {
    if (cancel.IsCancelled()) break;

    auto resp_or = conv->SendMessageSync(message_json);
    if (!resp_or.ok()) {
      throw AgentflowError("AgentNode: Conversation::SendMessageSync failed: " +
                            std::string(resp_or.status().message()));
    }
    const std::string& resp_str = *resp_or;
    if (cfg_.stream_tokens && !resp_str.empty()) {
      // No real token streaming on the sync path; emit the whole response
      // as a single chunk for trace observability.
      emit.EmitToken(Id(), resp_str);
    }

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
        std::string name = tc.value("name", tc.value("function",
                                                      json::object())
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
        std::string result = co_await DispatchTool(name, args, cancel, emit);
        // Per Gemma4 jinja template: each content item has `name` + `response`
        // fields directly; engine renders <|tool_response>response:NAME{...}<tool_response|>.
        tool_content.push_back({
          {"name", name},
          {"response", {{"value", result}}},
        });
      }
      json tool_message = {{"role", "tool"}, {"content", tool_content}};
      message_json = tool_message.dump();
      continue;
    }

    // No tool calls — extract the assistant text and we're done.
    final_answer = ExtractAssistantText(resp);
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
