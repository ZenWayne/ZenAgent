// agentflow/observability/jsonl_event_emitter.h
#ifndef AGENTFLOW_OBSERVABILITY_JSONL_EVENT_EMITTER_H_
#define AGENTFLOW_OBSERVABILITY_JSONL_EVENT_EMITTER_H_

#include <iosfwd>
#include <mutex>

#include "agentflow/core/event.h"

namespace agentflow {

// Writes each TraceEvent as one JSON line to `out`. Thread-safe via internal
// mutex; one emitter can be shared across coroutines and threads. Caller owns
// the stream and must keep it alive at least as long as the emitter.
//
// Format: protobuf JSON with stable field presence
// (always_print_fields_with_no_presence = true, add_whitespace = false).
// Each Emit produces exactly one line ending
// in '\n'. Designed for use with `jq`, downstream log shippers, and the JNI
// trace bridge.
class JsonlEventEmitter : public EventEmitter {
 public:
  explicit JsonlEventEmitter(std::ostream& out);
  void Emit(proto::TraceEvent ev) override;

 private:
  std::ostream& out_;
  std::mutex mu_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_OBSERVABILITY_JSONL_EVENT_EMITTER_H_
