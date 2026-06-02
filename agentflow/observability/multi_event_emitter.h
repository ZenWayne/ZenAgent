// agentflow/observability/multi_event_emitter.h
#ifndef AGENTFLOW_OBSERVABILITY_MULTI_EVENT_EMITTER_H_
#define AGENTFLOW_OBSERVABILITY_MULTI_EVENT_EMITTER_H_

#include <vector>

#include "agentflow/core/event.h"

namespace agentflow {

// Fan-out emitter: forwards each event to every child in submission order.
// Children are non-owning pointers (matching the Runner::Options::trace
// convention); the caller is responsible for child lifetimes.
//
// The first N-1 children receive a copy; the last receives a move. Empty
// children list is a no-op.
class MultiEventEmitter : public EventEmitter {
 public:
  explicit MultiEventEmitter(std::vector<EventEmitter*> children);
  void Emit(proto::TraceEvent ev) override;

 private:
  std::vector<EventEmitter*> children_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_OBSERVABILITY_MULTI_EVENT_EMITTER_H_
