// agentflow/inference/openai/stream_accumulator.h
#ifndef AGENTFLOW_INFERENCE_OPENAI_STREAM_ACCUMULATOR_H_
#define AGENTFLOW_INFERENCE_OPENAI_STREAM_ACCUMULATOR_H_

#include <map>
#include <string>
#include <string_view>

namespace agentflow::openai {

// Accumulates OpenAI streaming frames into one canonical assistant message.
//
// Tool-call arguments arrive fragmented: for a given `index`, the first frame
// carries `id` and `function.name`, and every later frame carries only another
// piece of `function.arguments` to concatenate. Getting this wrong produces
// truncated or interleaved JSON arguments, so it is covered exhaustively by
// stream_accumulator_test.
//
// Malformed frames are ignored rather than treated as errors — a stray
// keep-alive must not abort a half-finished answer.
class StreamAccumulator {
 public:
  // Feeds one SSE data payload (already stripped; never "[DONE]").
  // Returns the text delta this frame contained, or "" if it carried none.
  std::string Feed(std::string_view frame_json);

  // The canonical assistant JSON for everything fed so far.
  std::string Canonical() const;

 private:
  struct PartialCall {
    std::string id;
    std::string name;
    std::string arguments;
  };

  std::string text_;
  // Keyed by the stream's `index` so parallel calls stay separate and ordered.
  std::map<int, PartialCall> calls_;
};

}  // namespace agentflow::openai
#endif  // AGENTFLOW_INFERENCE_OPENAI_STREAM_ACCUMULATOR_H_
