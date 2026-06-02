// agentflow/observability/callback_event_emitter.h
#ifndef AGENTFLOW_OBSERVABILITY_CALLBACK_EVENT_EMITTER_H_
#define AGENTFLOW_OBSERVABILITY_CALLBACK_EVENT_EMITTER_H_

#include <functional>
#include <utility>

#include "agentflow/core/event.h"

namespace agentflow {

// Forwards every TraceEvent to a std::function. The natural shape for
// in-process consumers: tests capturing events into a vector, JNI bridges
// marshalling onto a Kotlin Flow executor, UI layers that want to observe
// streaming tokens without a file in the middle.
//
// No internal locking. If a caller wires this into a multi-threaded driver
// they must make the callback itself thread-safe. Today's Runner is single-
// threaded so this is moot.
//
// The callback is required to be non-null at construction.
class CallbackEventEmitter : public EventEmitter {
 public:
  using Callback = std::function<void(const proto::TraceEvent&)>;
  explicit CallbackEventEmitter(Callback cb) : cb_(std::move(cb)) {}
  void Emit(proto::TraceEvent ev) override { cb_(ev); }

 private:
  Callback cb_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_OBSERVABILITY_CALLBACK_EVENT_EMITTER_H_
