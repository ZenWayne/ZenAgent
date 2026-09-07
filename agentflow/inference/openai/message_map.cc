// agentflow/inference/openai/message_map.cc
#include "agentflow/inference/openai/message_map.h"

#include <algorithm>
#include <functional>

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
  // Thinking-mode round-trip: DeepSeek v4 requires the previous turn's
  // reasoning_content to be passed back when that turn emitted tool_calls.
  if (m.contains("reasoning_content") && m["reasoning_content"].is_string()) {
    msg["reasoning_content"] = m["reasoning_content"].get<std::string>();
  }
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

void RepairToolCallPairing(std::vector<nlohmann::json>* messages) {
  if (messages == nullptr) return;
  // Placeholder for a call whose result never arrived. Shaped like every other
  // in-band tool failure the agent already feeds back (an {"error": ...}
  // object), so the model reads it as a failed call rather than as data.
  static constexpr char kNotExecuted[] =
      R"({"error":"tool call was not executed: the turn ended before the )"
      R"(result was produced"})";

  for (size_t i = 0; i < messages->size(); ++i) {
    const json& m = (*messages)[i];
    if (!m.is_object() || m.value("role", "") != "assistant") continue;
    auto calls = m.find("tool_calls");
    if (calls == m.end() || !calls->is_array() || calls->empty()) continue;

    // The ids this assistant turn asked to have answered, in order.
    std::vector<std::string> want;
    for (const auto& c : *calls) {
      if (c.is_object() && c.contains("id") && c["id"].is_string()) {
        const auto& id = c["id"].get_ref<const std::string&>();
        // An empty id cannot be paired with anything; ToOpenAiMessages already
        // refuses to build a tool result for one, so leave it alone here.
        if (!id.empty()) want.push_back(id);
      }
    }

    // The run of tool messages directly after it is the only place a result
    // may live — the provider stops looking at the first non-tool message.
    size_t end = i + 1;
    std::vector<std::string> answered;
    while (end < messages->size() && (*messages)[end].is_object() &&
           (*messages)[end].value("role", "") == "tool") {
      answered.push_back((*messages)[end].value("tool_call_id", ""));
      ++end;
    }

    std::vector<json> missing;
    for (const auto& id : want) {
      if (std::find(answered.begin(), answered.end(), id) == answered.end()) {
        missing.push_back({{"role", "tool"},
                           {"tool_call_id", id},
                           {"content", kNotExecuted}});
      }
    }
    if (!missing.empty()) {
      messages->insert(messages->begin() + end, missing.begin(),
                       missing.end());
    }
    // Skip the whole paired run; ++i then lands on the message after it.
    i = end + missing.size() - 1;
  }
}

// DeepSeek strict-mode normalization — family-specialized tool passing.
// Per https://api-docs.deepseek.com/zh-cn/guides/tool_calls#strict-模式beta:
//   every object's properties must ALL be `required` and `additionalProperties
//   : false`; unsupported keywords (minLength/maxLength/minItems/maxItems)
//   must be dropped; objects with NO properties cannot be strict at all
//   (verified against /beta: "An object with no properties is not allowed" —
//   even inside anyOf, even with additionalProperties:false). The accepted
//   strategy for such tools is to LEAVE THEM NON-STRICT (schema verbatim):
//   the beta endpoint accepts a mixed array of strict + non-strict functions
//   (12 strict + 5 non-strict verified ACCEPTED).
//
// Family gate: OTHER model families must never be normalized like this —
// `strict_tools` only takes effect for the deepseek family (BuildRequestBody).
json NormalizeStrictSchema(json func) {
  // Scope: full-recursive check whether ANY object node (at any depth, in any
  // dict/array value) has type==object but no properties dict. Strict-mode
  // beta rejects such nodes; tools carrying them fall back to non-strict.
  auto HasEmptyObject = [](const json& p) -> bool {
    std::function<bool(const json&)> scan = [&](const json& node) -> bool {
      if (node.is_array()) {
        for (const auto& v : node)
          if (scan(v)) return true;
        return false;
      }
      if (!node.is_object()) return false;
      if (node.value("type", "") == "object") {
        const auto& props = node.find("properties");
        if (props == node.end() || !props->is_object() ||
            props->empty()) {
          return true;
        }
      }
      for (const auto& [k, v] : node.items()) {
        if (scan(v)) return true;
      }
      return false;
    };
    return scan(p);
  };

  // Top level: no parameterized properties at all (e.g. list_projects) —
  // nothing to mark required; stay non-strict.
  if (func.contains("parameters") && func["parameters"].is_object()) {
    const auto& params = func["parameters"];
    bool has_props = params.contains("properties") && params["properties"].is_object() &&
                     !params["properties"].empty();
    if (!has_props) return func;
    // Nested empty object anywhere → this whole function cannot be strict.
    if (HasEmptyObject(params)) return func;
  }
  func["strict"] = true;
  if (func.contains("parameters") && func["parameters"].is_object()) {
    std::function<void(json&)> recurse = [&](json& p) -> void {
      if (!p.is_object()) return;
      if (p.contains("properties") && p["properties"].is_object()) {
        std::vector<std::string> req;
        for (auto& [key, val] : p["properties"].items()) {
          req.push_back(key);
          // Delete DeepSeek-unsupported validation keywords from every
          // property node (strict schema validation rejects them).
          if (val.is_object()) {
            for (const char* k : {"minLength", "maxLength", "minItems",
                                  "maxItems"}) {
              val.erase(k);
            }
            if (val.value("type", "") == "object") recurse(val);
            if (val.contains("items") && val["items"].is_object()) {
              recurse(val["items"]);
            }
          }
        }
        p["required"] = req;
      }
      p["additionalProperties"] = false;
    };
    recurse(func["parameters"]);
  }
  return func;
}

bool IsDeepSeekFamily(std::string_view model) {
  // Family gate: deepseek-v4-*, deepseek-chat, deepseek-reasoner...
  return model.rfind("deepseek", 0) == 0;
}

std::string BuildRequestBody(std::string_view model,
                              const ChatConversationOptions& opts,
                              const std::vector<nlohmann::json>& messages,
                              bool stream, bool strict_tools) {
  json body;
  body["model"] = std::string(model);
  body["messages"] = messages;
  body["stream"] = stream;
  if (opts.max_output_tokens > 0) body["max_tokens"] = opts.max_output_tokens;

  json tools = json::parse(opts.tools_json, nullptr, false);
  if (!tools.is_discarded() && tools.is_array() && !tools.empty()) {
    // Family-specialized: only DeepSeek models get strict-mode tool passing.
    // Other OpenAI-compatible endpoints pass tools through verbatim — strict
    // kwargs/additionalProperties constraints are DeepSeek-specific (Beta).
    if (strict_tools && IsDeepSeekFamily(model)) {
      for (auto& t : tools) {
        if (t.is_object() && t.contains("function") &&
            t["function"].is_object()) {
          t["function"] = NormalizeStrictSchema(t["function"]);
        }
      }
    }
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
  // Non-streaming path: preserve thinking-mode reasoning_content too.
  if (msg.contains("reasoning_content") &&
      msg["reasoning_content"].is_string()) {
    out["reasoning_content"] = msg["reasoning_content"].get<std::string>();
  }

  if (msg.contains("tool_calls") && msg["tool_calls"].is_array() &&
      !msg["tool_calls"].empty()) {
    out["tool_calls"] = msg["tool_calls"];
  }
  return out.dump();
}

}  // namespace agentflow::openai
