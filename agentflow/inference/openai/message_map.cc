// agentflow/inference/openai/message_map.cc
#include "agentflow/inference/openai/message_map.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

// Flattens a canonical content array into a single string. Canonical content
// is [{"type":"text","text":"..."}]; OpenAI wants a plain string.
std::string FlattenContent(const json& content) {
  if (content.is_string()) return content.get<std::string>();
  if (!content.is_array()) return {};
  std::string out;
  for (const auto& item : content) {
    // json::value() THROWS type_error.306 on a non-object and
    // type_error.302 when a present key has the wrong type (e.g.
    // {"type":42}). Remote model output is untrusted — never read a field
    // without proving BOTH that its container is an object AND that the
    // field has the type we are about to read it as. Same shape as
    // StreamAccumulator::Feed.
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

}  // namespace

std::optional<nlohmann::json> SystemMessage(
    std::string_view system_message_json) {
  if (system_message_json.empty()) return std::nullopt;
  json arr = json::parse(system_message_json, nullptr, false);
  if (arr.is_discarded()) return std::nullopt;
  std::string text = FlattenContent(arr);
  if (text.empty()) return std::nullopt;
  return json{{"role", "system"}, {"content", std::move(text)}};
}

absl::StatusOr<std::vector<nlohmann::json>> ToOpenAiMessages(
    std::string_view canonical_message_json) {
  json m = json::parse(canonical_message_json, nullptr, false);
  if (m.is_discarded() || !m.is_object()) {
    return absl::InvalidArgumentError("canonical message is not a JSON object");
  }
  std::string role;
  if (m.contains("role") && m["role"].is_string()) {
    role = m["role"].get<std::string>();
  }
  std::vector<json> out;

  if (role == "tool") {
    if (!m.contains("content") || !m["content"].is_array()) {
      return absl::InvalidArgumentError("tool message has no content array");
    }
    for (const auto& entry : m["content"]) {
      // json::value() THROWS type_error.306 on a non-object and
      // type_error.302 on a present-but-wrong-typed key. Same guard as
      // FlattenContent — this loop must not be the one place that omits it.
      if (!entry.is_object()) continue;
      std::string id;
      if (entry.contains("id") && entry["id"].is_string()) {
        id = entry["id"].get<std::string>();
      }
      if (id.empty()) {
        std::string name = "?";
        if (entry.contains("name") && entry["name"].is_string()) {
          name = entry["name"].get<std::string>();
        }
        return absl::InvalidArgumentError(absl::StrCat(
            "tool result for '", name,
            "' has no id; OpenAI requires tool_call_id on every tool message"));
      }
      std::string value;
      if (entry.contains("response") && entry["response"].contains("value")) {
        const auto& v = entry["response"]["value"];
        value = v.is_string() ? v.get<std::string>() : v.dump();
      }
      out.push_back({{"role", "tool"},
                     {"tool_call_id", id},
                     {"content", std::move(value)}});
    }
    return out;
  }

  json msg = {{"role", role.empty() ? "user" : role}};
  msg["content"] = m.contains("content") ? FlattenContent(m["content"])
                                          : std::string{};
  if (m.contains("tool_calls") && m["tool_calls"].is_array() &&
      !m["tool_calls"].empty()) {
    json calls = json::array();
    for (const auto& tc : m["tool_calls"]) {
      json call = tc;
      // OpenAI requires an explicit type discriminator; LiteRT-LM omits it.
      if (!call.contains("type")) call["type"] = "function";
      calls.push_back(std::move(call));
    }
    msg["tool_calls"] = std::move(calls);
  }
  out.push_back(std::move(msg));
  return out;
}

std::string BuildRequestBody(std::string_view model,
                              const ChatConversationOptions& opts,
                              const std::vector<nlohmann::json>& messages,
                              bool stream) {
  json body;
  body["model"] = std::string(model);
  body["messages"] = messages;
  body["stream"] = stream;
  if (opts.max_output_tokens > 0) body["max_tokens"] = opts.max_output_tokens;

  json tools = json::parse(opts.tools_json, nullptr, false);
  if (!tools.is_discarded() && tools.is_array() && !tools.empty()) {
    body["tools"] = std::move(tools);
  }
  return body.dump();
}

absl::StatusOr<std::string> ResponseToCanonical(std::string_view body) {
  json resp = json::parse(body, nullptr, false);
  if (resp.is_discarded()) {
    return absl::InternalError("OpenAI response is not valid JSON");
  }
  if (!resp.contains("choices") || !resp["choices"].is_array() ||
      resp["choices"].empty()) {
    return absl::InternalError("OpenAI response has no choices");
  }
  // A provider can return anything. Indexing a non-object with a string key
  // throws type_error.305, so every hop is type-checked before it is taken.
  // A choice without a message object is an error, not an empty answer —
  // fabricating "" here would hand the agent a silent blank turn.
  const json& choice = resp["choices"][0];
  if (!choice.is_object() || !choice.contains("message") ||
      !choice["message"].is_object()) {
    return absl::InternalError(
        "OpenAI response choice has no message object");
  }
  const json& msg = choice["message"];

  json out = {{"role", "assistant"}};
  std::string text;
  if (msg.contains("content") && msg["content"].is_string()) {
    text = msg["content"].get<std::string>();
  }
  out["content"] = json::array({{{"type", "text"}, {"text", text}}});

  if (msg.contains("tool_calls") && msg["tool_calls"].is_array() &&
      !msg["tool_calls"].empty()) {
    out["tool_calls"] = msg["tool_calls"];
  }
  return out.dump();
}

}  // namespace agentflow::openai
