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

// Restores the tool-call pairing invariant on a conversation history, in place.
//
// An assistant message carrying `tool_calls` MUST be followed by one tool
// message per `tool_call_id`; a provider rejects anything else with
//   400 An assistant message with 'tool_calls' must be followed by tool
//       messages responding to each 'tool_call_id'.
// The assistant message enters history the moment its stream completes, but a
// turn can end before the results are fed back — the ReAct loop hits max_iter,
// the client aborts, tool dispatch throws. History then holds an unanswered
// tool_calls, and since the whole history is resent on every later turn, the
// SESSION is poisoned, not just the turn that broke: every subsequent message
// fails the same way until the session is discarded.
//
// Missing results are filled with an explicit "not executed" placeholder
// rather than dropped: the assistant's own text/reasoning stays intact, and
// the model can see the call never ran instead of silently believing it did.
// An already-paired history is left untouched, so this is idempotent.
void RepairToolCallPairing(std::vector<nlohmann::json>* messages);

// Builds the request body. `opts.tools_json` is already the OpenAI tools shape
// (AgentNode::BuildToolsJson emits it), so it is passed through verbatim; an
// empty array is omitted entirely.
std::string BuildRequestBody(std::string_view model,
                              const ChatConversationOptions& opts,
                              const std::vector<nlohmann::json>& messages,
                              bool stream, bool strict_tools = false);

// Converts a NON-streaming /v1/chat/completions response body into canonical
// assistant JSON. (The streaming path uses StreamAccumulator instead.)
absl::StatusOr<std::string> ResponseToCanonical(std::string_view body);

}  // namespace agentflow::openai
#endif  // AGENTFLOW_INFERENCE_OPENAI_MESSAGE_MAP_H_
