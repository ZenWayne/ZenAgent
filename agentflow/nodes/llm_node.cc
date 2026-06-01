// agentflow/nodes/llm_node.cc
#include "agentflow/nodes/llm_node.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "agentflow/core/errors.h"
#include "agentflow/inference/litert_lm_session.h"
#include "c/engine.h"

namespace agentflow {

namespace {

using json = nlohmann::json;

// Mirror of the protobuf-field reflection helpers AgentNode uses. If a third
// node needs them they can be lifted into agentflow/nodes/state_field_util.h.
std::string ReadField(const State& s, const std::string& f) {
  const auto* msg = s.UnsafeMessage();
  if (!msg) return {};
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(f);
  if (!desc) return {};
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    return refl->GetString(*msg, desc);
  }
  return {};
}

void WriteField(State& s, const std::string& f, const std::string& v) {
  auto* msg = const_cast<google::protobuf::Message*>(s.UnsafeMessage());
  if (!msg) return;
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(f);
  if (!desc) return;
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    refl->SetString(msg, desc, v);
  }
}

}  // namespace

LlmNode::LlmNode(LlmNodeConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.id.empty()) throw AgentflowError("LlmNode: id required");
  if (!cfg_.engine) throw AgentflowError("LlmNode: engine required");
  if (!cfg_.io_ctx) throw AgentflowError("LlmNode: io_ctx required");
}

std::string LlmNode::BuildConversationJson(const State& state) const {
  json msgs = json::array();
  if (!cfg_.system_prompt.empty()) {
    msgs.push_back({{"role", "system"}, {"content", cfg_.system_prompt}});
  }
  // (messages_field history append left as a follow-up; AgentNode no-ops the
  // same way today — keep parity for now.)
  msgs.push_back(
      {{"role", "user"}, {"content", ReadField(state, cfg_.input_field)}});

  json full;
  full["messages"] = msgs;
  full["max_tokens"] = cfg_.max_output_tokens;
  full["stream"] = cfg_.stream_tokens;

  // Publish tool schemas so the model can emit function-calling JSON. We do
  // not dispatch the call here — that's the caller's job.
  if (cfg_.tool_registry) {
    auto tools_json = cfg_.tool_registry->ExportToolsJson(cfg_.tool_names);
    full["tools"] = json::parse(tools_json);
  }
  return full.dump();
}

void LlmNode::WriteOutput(State& state, const std::string& text) const {
  WriteField(state, cfg_.output_field, text);
}

asio::awaitable<State> LlmNode::Run(State state, const CancelToken& cancel,
                                     EventEmitter& emit) {
  if (cancel.IsCancelled()) co_return std::move(state);

  auto* raw_session = litert_lm_engine_create_session(
      cfg_.engine->Get(), /*session_config=*/nullptr);
  if (!raw_session) {
    throw AgentflowError("LlmNode: failed to create LiteRT-LM session");
  }
  LiteRtLmSession session(raw_session, *cfg_.io_ctx);

  emit.EmitNodeStart(Id());
  session.Start(BuildConversationJson(state));

  std::string accum;
  while (true) {
    if (cancel.IsCancelled()) break;
    std::string tok = co_await session.NextTokenAsync();
    if (tok.empty()) break;
    accum += tok;
    if (cfg_.stream_tokens) emit.EmitToken(Id(), tok);
  }
  emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);

  WriteOutput(state, accum);
  co_return std::move(state);
}

}  // namespace agentflow
