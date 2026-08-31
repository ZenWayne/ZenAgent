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
    // json::value() THROWS type_error.306 on a non-object and
    // type_error.302 when a present key has the wrong type (e.g.
    // {"type":42}), and allow_exceptions=false above does not cover either.
    // Model output is untrusted — never read a field without proving BOTH
    // that its container is an object AND that the field has the type we
    // are about to read it as.
    if (!item.is_object()) continue;
    if (!item.contains("type") || !item["type"].is_string() ||
        item["type"].get<std::string>() != "text") {
      continue;
    }
    if (item.contains("text") && item["text"].is_string()) {
      out.append(item["text"].get<std::string>());
    }
  }
  return out;
}

std::string DecodeGemmaQuoteTokens(std::string_view response_json) {
  // The token appears as the 6-character sequence <|\"|> inside tool-call
  // argument JSON: the model emitted its open_quote/close_quote token in
  // place of the JSON string delimiters, and the engine embedded the decoded
  // token text (with its quote escaped) as value content. Deleting the
  // sequence restores the intended argument values.
  constexpr std::string_view kQuoteToken = "<|\\\"|>";
  if (response_json.find(kQuoteToken) == std::string_view::npos) {
    return std::string(response_json);
  }
  std::string out;
  out.reserve(response_json.size());
  size_t pos = 0;
  for (;;) {
    const size_t found = response_json.find(kQuoteToken, pos);
    if (found == std::string_view::npos) {
      out.append(response_json.substr(pos));
      break;
    }
    out.append(response_json.substr(pos, found - pos));
    pos = found + kQuoteToken.size();
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
