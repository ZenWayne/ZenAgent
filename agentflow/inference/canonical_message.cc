// agentflow/inference/canonical_message.cc
#include "agentflow/inference/canonical_message.h"

#include <nlohmann/json.hpp>

namespace agentflow {
namespace {
using json = nlohmann::json;
}  // namespace

std::string ExtractAssistantText(std::string_view canonical_json) {
  json resp = json::parse(canonical_json, nullptr, /*allow_exceptions=*/false);
  if (resp.is_discarded() || !resp.contains("content")) return {};
  const auto& content = resp["content"];
  if (!content.is_array()) return {};
  std::string out;
  for (const auto& item : content) {
    // json::value() THROWS type_error.306 on a non-object, and
    // allow_exceptions=false above does not cover it. Model output is
    // untrusted, so skip anything that is not an object.
    if (!item.is_object()) continue;
    if (item.value("type", "") == "text" && item.contains("text") &&
        item["text"].is_string()) {
      out.append(item["text"].get<std::string>());
    }
  }
  return out;
}

void LiteRtStreamAssembler::Feed(std::string_view chunk) {
  if (chunk.empty()) return;

  json cj = json::parse(chunk, nullptr, /*allow_exceptions=*/false);
  if (cj.is_discarded()) {
    // Raw, non-JSON chunk — the engine emitted plain text.
    deltas_.emplace_back(chunk);
    text_.append(chunk);
    return;
  }

  if (cj.contains("tool_calls") && cj["tool_calls"].is_array() &&
      !cj["tool_calls"].empty()) {
    tool_call_json_ = cj.dump();
    return;
  }

  std::string delta = ExtractAssistantText(chunk);
  if (!delta.empty()) {
    deltas_.push_back(delta);
    text_.append(delta);
  }
}

std::string LiteRtStreamAssembler::Canonical() const {
  if (!tool_call_json_.empty()) return tool_call_json_;
  json msg = {
      {"role", "assistant"},
      {"content", json::array({{{"type", "text"}, {"text", text_}}})},
  };
  return msg.dump();
}

}  // namespace agentflow
