// agentflow/inference/openai/message_map.h
//
// Pure conversion between agentflow's canonical message shape (which is
// LiteRT-LM's) and the OpenAI /v1/chat/completions shape. No I/O.
#ifndef AGENTFLOW_INFERENCE_OPENAI_MESSAGE_MAP_H_
#define AGENTFLOW_INFERENCE_OPENAI_MESSAGE_MAP_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include <nlohmann/json.hpp>

#include "agentflow/inference/chat_backend.h"

namespace agentflow::openai {

// ChatConversationOptions.system_message_json is a BARE content array
// ([{"type":"text","text":"..."}]), not a {role,content} object — LiteRT-LM
// wraps it itself. Returns nullopt when empty or unparseable.
std::optional<nlohmann::json> SystemMessage(
    std::string_view system_message_json);

// Converts ONE canonical message into 1..N OpenAI messages.
//
// The one-to-many case is `role:"tool"`: a single canonical tool message can
// carry several results, and OpenAI requires each to be its own message with
// its own tool_call_id. A result entry missing `id` is an InvalidArgumentError
// rather than a request the server will reject opaquely.
absl::StatusOr<std::vector<nlohmann::json>> ToOpenAiMessages(
    std::string_view canonical_message_json);

// Builds the request body. `opts.tools_json` is already the OpenAI tools shape
// (AgentNode::BuildToolsJson emits it), so it is passed through verbatim; an
// empty array is omitted entirely.
std::string BuildRequestBody(std::string_view model,
                              const ChatConversationOptions& opts,
                              const std::vector<nlohmann::json>& messages,
                              bool stream);

// Converts a NON-streaming /v1/chat/completions response body into canonical
// assistant JSON. (The streaming path uses StreamAccumulator instead.)
absl::StatusOr<std::string> ResponseToCanonical(std::string_view body);

}  // namespace agentflow::openai
#endif  // AGENTFLOW_INFERENCE_OPENAI_MESSAGE_MAP_H_
