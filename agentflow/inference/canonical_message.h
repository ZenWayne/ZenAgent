// agentflow/inference/canonical_message.h
//
// Pure helpers over the canonical assistant-message shape. No engine, no
// network, no io_context — every function here is directly unit-testable.
//
// The canonical shape is LiteRT-LM's existing response shape:
//   {"role":"assistant",
//    "content":[{"type":"text","text":"..."}],
//    "tool_calls":[{"id":"...","function":{"name":"...","arguments":"{...}"}}]}
#ifndef AGENTFLOW_INFERENCE_CANONICAL_MESSAGE_H_
#define AGENTFLOW_INFERENCE_CANONICAL_MESSAGE_H_

#include <string>
#include <string_view>
#include <vector>

namespace agentflow {

// Concatenates every {"type":"text"} item in `canonical_json`'s content array.
// Returns "" if the input is not parseable, has no content array, or holds no
// text items.
std::string ExtractAssistantText(std::string_view canonical_json);

// Accumulates LiteRT-LM stream envelopes into one canonical assistant message.
//
// LiteRT-LM streams each delta as a FULL message envelope wrapping one
// incremental piece, e.g.
//   {"role":"assistant","content":[{"type":"text","text":"The"}]}
// for text, or a complete
//   {"role":"assistant","tool_calls":[...]}
// for a tool call. Raw concatenation of envelopes is not valid JSON, so this
// class rebuilds a single canonical message instead.
class LiteRtStreamAssembler {
 public:
  // Feeds one stream chunk. A chunk that does not parse as JSON is treated as
  // a plain text delta (the engine occasionally emits raw text).
  void Feed(std::string_view chunk);

  // Text deltas seen so far, in arrival order. The caller forwards these to a
  // TokenSink / EventEmitter.
  const std::vector<std::string>& text_deltas() const { return deltas_; }

  // The canonical assistant JSON for everything fed so far. If any chunk
  // carried tool_calls, that message is returned (a tool call IS the turn's
  // response); otherwise a text message built from the accumulated deltas.
  std::string Canonical() const;

 private:
  std::vector<std::string> deltas_;
  std::string text_;
  std::string tool_call_json_;  // empty when no tool call was seen
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_CANONICAL_MESSAGE_H_
