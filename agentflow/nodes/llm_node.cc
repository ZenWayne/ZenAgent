// agentflow/nodes/llm_node.cc
#include "agentflow/nodes/llm_node.h"

#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "absl/status/status.h"

#include "agentflow/core/errors.h"
#include "agentflow/inference/canonical_message.h"

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
  if (!cfg_.backend) throw AgentflowError("LlmNode: backend required");
  if (!cfg_.io_ctx) throw AgentflowError("LlmNode: io_ctx required");
}

void LlmNode::WriteOutput(State& state, const std::string& text) const {
  WriteField(state, cfg_.output_field, text);
}

asio::awaitable<State> LlmNode::Run(State state, const CancelToken& cancel,
                                     EventEmitter& emit) {
  if (cancel.IsCancelled()) co_return std::move(state);

  ChatConversationOptions opts;
  if (!cfg_.system_prompt.empty()) {
    // A BARE content array — the backend wraps it into {role:system,...}.
    json sys = json::array({{{"type", "text"}, {"text", cfg_.system_prompt}}});
    opts.system_message_json = sys.dump();
  }
  // Publish tool schemas so the model can emit function-calling JSON. LlmNode
  // never dispatches a call — the raw reply is left for the next node.
  if (cfg_.tool_registry) {
    opts.tools_json = cfg_.tool_registry->ExportToolsJson(cfg_.tool_names);
  }
  opts.max_output_tokens = cfg_.max_output_tokens;

  auto conv = cfg_.backend->CreateConversation(std::move(opts));
  if (!conv) {
    throw AgentflowError("LlmNode: failed to create conversation on backend " +
                          std::string(cfg_.backend->Describe()));
  }
  cancel.OnCancel([conv]() { conv->Cancel(); });

  TokenSink sink;
  if (cfg_.stream_tokens) {
    sink = [this, &emit](std::string_view delta) -> asio::awaitable<void> {
      emit.EmitToken(Id(), delta);
      co_return;
    };
  }

  json user = {
      {"role", "user"},
      {"content", json::array({{{"type", "text"},
                                {"text", ReadField(state, cfg_.input_field)}}})},
  };
  auto resp_or = co_await conv->SendAsync(user.dump(), sink, cancel);
  if (!resp_or.ok()) {
    if (absl::IsCancelled(resp_or.status())) co_return std::move(state);
    throw AgentflowError("LlmNode: backend send failed: " +
                          std::string(resp_or.status().message()));
  }

  // The raw reply may carry tool_calls; the next node interprets it. When it
  // is plain text, write the extracted text rather than the JSON envelope.
  std::string text = ExtractAssistantText(*resp_or);
  WriteOutput(state, text.empty() ? *resp_or : text);
  co_return std::move(state);
}

}  // namespace agentflow
