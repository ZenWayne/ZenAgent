// agentflow/nodes/agent_node.cc
#include "agentflow/nodes/agent_node.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "agentflow/core/errors.h"
#include "agentflow/inference/litert_lm_session.h"
#include "c/engine.h"

namespace agentflow {

namespace {

using json = nlohmann::json;

std::string ReadField(const State& state, const std::string& field_name) {
  const auto* msg = state.UnsafeMessage();
  if (!msg) return {};
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(field_name);
  if (!desc) return {};
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    return refl->GetString(*msg, desc);
  }
  return {};
}

void WriteField(State& state, const std::string& field_name,
                const std::string& value) {
  auto* msg = const_cast<google::protobuf::Message*>(state.UnsafeMessage());
  if (!msg) return;
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(field_name);
  if (!desc) return;
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    refl->SetString(msg, desc, value);
  }
}

void AppendMessage(State& state, const std::string& field_name,
                   const json& message_obj) {
  auto* msg = const_cast<google::protobuf::Message*>(state.UnsafeMessage());
  if (!msg) return;
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(field_name);
  if (!desc || !desc->is_repeated()) return;
  if (desc->type() != google::protobuf::FieldDescriptor::TYPE_MESSAGE) return;

  const auto* entry_desc = desc->message_type();
  auto* entry = refl->AddMessage(msg, desc);
  auto* role_f = entry_desc->FindFieldByName("role");
  auto* content_f = entry_desc->FindFieldByName("content");
  if (role_f && content_f) {
    refl->SetString(entry, role_f, message_obj["role"].get<std::string>());
    refl->SetString(entry, content_f, message_obj["content"].get<std::string>());
    if (message_obj.contains("tool_call_id")) {
      auto* tcid_f = entry_desc->FindFieldByName("tool_call_id");
      if (tcid_f)
        refl->SetString(entry, tcid_f, message_obj["tool_call_id"].get<std::string>());
    }
  }
}

}  // namespace

AgentNode::AgentNode(AgentNodeConfig cfg)
    : cfg_(std::move(cfg)),
      id_("agent") {}

std::string AgentNode::BuildConversationJson(const State& state) const {
  json msgs = json::array();

  if (!cfg_.system_prompt.empty()) {
    msgs.push_back({{"role", "system"}, {"content", cfg_.system_prompt}});
  }

  std::string input = ReadField(state, cfg_.input_field);
  msgs.push_back({{"role", "user"}, {"content", input}});

  // Attach tools if configured
  json full;
  full["messages"] = msgs;
  full["max_tokens"] = cfg_.max_output_tokens;
  full["stream"] = true;

  return full.dump();
}

void AgentNode::WriteOutput(State& state, const std::string& text) const {
  WriteField(state, cfg_.output_field, text);
}

asio::awaitable<State> AgentNode::Run(
    State state, const CancelToken& cancel, EventEmitter& emit) {
  if (!cfg_.engine || !cfg_.io_ctx) {
    throw AgentflowError("AgentNode: engine and io_ctx must be configured");
  }

  auto* raw_session = litert_lm_engine_create_session(
      cfg_.engine->Get(),
      /*session_config=*/nullptr);
  if (!raw_session) {
    throw AgentflowError("AgentNode: failed to create LiteRT-LM session");
  }

  LiteRtLmSession session(raw_session, *cfg_.io_ctx);
  if (cancel.IsCancelled()) co_return std::move(state);

  std::string final_answer;
  for (int iter = 0; iter < cfg_.max_iter; ++iter) {
    if (cancel.IsCancelled()) break;

    emit.EmitNodeStart(Id());
    std::string conversation_json = BuildConversationJson(state);

    session.Start(conversation_json);
    std::string accum;
    while (true) {
      std::string token = co_await session.NextTokenAsync();
      if (token.empty()) break;
      accum += token;
      if (cfg_.stream_tokens) {
        emit.EmitToken(Id(), token);
      }
    }

    emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);

    // Check for tool call in output
    try {
      auto parsed = json::parse(accum);
      if (parsed.contains("tool_calls") && !parsed["tool_calls"].empty()) {
        for (const auto& tc : parsed["tool_calls"]) {
          std::string name = tc["function"]["name"];
          std::string args = tc["function"]["arguments"];
          co_await HandleToolCall(state, name, args, cancel);
        }
        continue;  // Loop back for next LLM call
      }
    } catch (const json::parse_error&) {
      // Not JSON = plain text output
    }

    // No tool call — this is the final answer
    final_answer = accum;
    WriteOutput(state, final_answer);
    break;
  }

  if (final_answer.empty() && !cancel.IsCancelled()) {
    WriteOutput(state, "Agent reached maximum iterations without a final answer.");
  }

  co_return std::move(state);
}

asio::awaitable<void> AgentNode::HandleToolCall(
    State& state, const std::string& name,
    const std::string& args, const CancelToken& cancel) {
  if (!cfg_.tool_registry) co_return;

  std::string result;
  try {
    result = co_await cfg_.tool_registry->Invoke(name, args, cancel);
  } catch (const std::exception& e) {
    result = std::string("Tool error: ") + e.what();
  }

  json tool_msg = {
    {"role", "tool"},
    {"tool_call_id", name},
    {"content", result},
  };
  AppendMessage(state, cfg_.messages_field, tool_msg);
}

}  // namespace agentflow
