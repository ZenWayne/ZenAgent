// agentflow/inference/openai/stream_accumulator.cc
#include "agentflow/inference/openai/stream_accumulator.h"

#include <nlohmann/json.hpp>

namespace agentflow::openai {
namespace {
using json = nlohmann::json;
}  // namespace

std::string StreamAccumulator::Feed(std::string_view frame_json) {
  json f = json::parse(frame_json, nullptr, /*allow_exceptions=*/false);
  if (f.is_discarded()) return {};
  if (!f.contains("choices") || !f["choices"].is_array() ||
      f["choices"].empty()) {
    return {};
  }
  const json& choice = f["choices"][0];
  if (!choice.contains("delta") || !choice["delta"].is_object()) return {};
  const json& delta = choice["delta"];

  std::string text_delta;
  if (delta.contains("content") && delta["content"].is_string()) {
    text_delta = delta["content"].get<std::string>();
    text_.append(text_delta);
  }

  if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
    for (const auto& tc : delta["tool_calls"]) {
      // Guard: an element of tool_calls that is not an object (stray
      // number/string/null from a malformed frame) would make
      // tc.value() throw type_error.306 — skip it instead of crashing.
      if (!tc.is_object()) continue;
      const int index = tc.value("index", 0);
      PartialCall& call = calls_[index];
      // id and name appear only in this index's FIRST frame; never overwrite
      // them with a later frame's absent value.
      if (tc.contains("id") && tc["id"].is_string()) {
        call.id = tc["id"].get<std::string>();
      }
      if (tc.contains("function") && tc["function"].is_object()) {
        const json& fn = tc["function"];
        if (fn.contains("name") && fn["name"].is_string()) {
          call.name = fn["name"].get<std::string>();
        }
        if (fn.contains("arguments") && fn["arguments"].is_string()) {
          // Fragment — append, never replace.
          call.arguments.append(fn["arguments"].get<std::string>());
        }
      }
    }
  }
  return text_delta;
}

std::string StreamAccumulator::Canonical() const {
  json out = {{"role", "assistant"}};
  out["content"] = json::array({{{"type", "text"}, {"text", text_}}});

  if (!calls_.empty()) {
    json arr = json::array();
    for (const auto& [index, call] : calls_) {  // std::map → ordered by index
      arr.push_back({{"id", call.id},
                     {"type", "function"},
                     {"function",
                      {{"name", call.name}, {"arguments", call.arguments}}}});
    }
    out["tool_calls"] = std::move(arr);
  }
  return out.dump();
}

}  // namespace agentflow::openai
