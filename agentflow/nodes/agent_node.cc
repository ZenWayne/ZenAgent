// agentflow/nodes/agent_node.cc
#include "agentflow/nodes/agent_node.h"

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

// Collect existing messages from the state's repeated message field into a JSON
// array. Each message is expected to have "role" and "content" string fields.
json ReadMessages(const State& state, const std::string& field_name) {
  json arr = json::array();
  const auto* msg = state.UnsafeMessage();
  if (!msg || field_name.empty()) return arr;

  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(field_name);
  if (!desc || !desc->is_repeated()) return arr;
  if (desc->type() != google::protobuf::FieldDescriptor::TYPE_MESSAGE) return arr;

  int size = refl->FieldSize(*msg, desc);
  const auto* entry_desc = desc->message_type();
  auto* role_f = entry_desc->FindFieldByName("role");
  auto* content_f = entry_desc->FindFieldByName("content");
  auto* tcid_f = entry_desc->FindFieldByName("tool_call_id");
  if (!role_f || !content_f) return arr;

  for (int i = 0; i < size; ++i) {
    const auto& entry = refl->GetRepeatedMessage(*msg, desc, i);
    json obj;
    obj["role"] = role_f->type() == google::protobuf::FieldDescriptor::TYPE_STRING
        ? refl->GetString(entry, role_f) : "";
    obj["content"] = content_f->type() == google::protobuf::FieldDescriptor::TYPE_STRING
        ? refl->GetString(entry, content_f) : "";
    if (tcid_f && tcid_f->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
      std::string tcid = refl->GetString(entry, tcid_f);
      if (!tcid.empty()) {
        obj["tool_call_id"] = tcid;
      }
    }
    arr.push_back(std::move(obj));
  }
  return arr;
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

  // Append existing conversation history (tool results, previous turns)
  json history = ReadMessages(state, cfg_.messages_field);
  for (auto& m : history) {
    msgs.push_back(std::move(m));
  }

  // Current user input — only include on first iteration (before any history exists)
  // to avoid duplicating the input on every ReAct turn.
  if (history.empty()) {
    std::string input = ReadField(state, cfg_.input_field);
    msgs.push_back({{"role", "user"}, {"content", input}});
  }

  json full;
  full["messages"] = msgs;
  full["max_tokens"] = cfg_.max_output_tokens;
  full["stream"] = cfg_.stream_tokens;

  // Attach tool definitions if configured
  if (cfg_.tool_registry) {
    full["tools"] = json::parse(
        cfg_.tool_registry->ExportToolsJson(cfg_.tool_names));
  }

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

  if (cancel.IsCancelled()) co_return std::move(state);

  std::string final_answer;
  for (int iter = 0; iter < cfg_.max_iter; ++iter) {
    if (cancel.IsCancelled()) break;

    auto* raw_session = litert_lm_engine_create_session(
        cfg_.engine->Get(),
        /*session_config=*/nullptr);
    if (!raw_session) {
      throw AgentflowError("AgentNode: failed to create LiteRT-LM session");
    }
    LiteRtLmSession session(raw_session, *cfg_.io_ctx);

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
    // LiteRT-LM format: {"tool_calls":[{"id":"...","function":{"name":"...","arguments":"..."}}]}
    try {
      auto parsed = json::parse(accum);
      if (parsed.contains("tool_calls") && !parsed["tool_calls"].empty()) {
        // Persist the assistant's tool-call message so the LLM sees its own
        // choices on subsequent iterations (standard ReAct pattern).
        json assistant_msg = parsed;
        assistant_msg["role"] = "assistant";
        AppendMessage(state, cfg_.messages_field, assistant_msg);

        for (const auto& tc : parsed["tool_calls"]) {
          std::string id = tc.value("id", "");
          std::string name = tc["function"]["name"];
          std::string args = tc["function"]["arguments"];
          co_await HandleToolCall(state, id, name, args, cancel, emit);
        }
        continue;  // Loop back for next LLM call
      }
    } catch (const json::exception&) {
      // Not a valid JSON tool-call response — treat as plain text output
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
    State& state, const std::string& call_id,
    const std::string& name,
    const std::string& args, const CancelToken& cancel,
    EventEmitter& emit) {
  if (!cfg_.tool_registry) co_return;

  emit.EmitToolCall(Id(), name, args);
  std::string result;
  try {
    result = co_await cfg_.tool_registry->Invoke(name, args, cancel);
  } catch (const std::exception& e) {
    result = std::string("Tool error: ") + e.what();
  }
  emit.EmitToolReturn(Id(), name, result);

  json tool_msg = {
    {"role", "tool"},
    {"tool_call_id", call_id.empty() ? name : call_id},
    {"content", result},
  };
  AppendMessage(state, cfg_.messages_field, tool_msg);
}

}  // namespace agentflow
