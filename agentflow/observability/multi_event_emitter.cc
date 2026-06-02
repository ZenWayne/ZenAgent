// agentflow/observability/multi_event_emitter.cc
#include "agentflow/observability/multi_event_emitter.h"

#include <utility>

namespace agentflow {

MultiEventEmitter::MultiEventEmitter(std::vector<EventEmitter*> children)
    : children_(std::move(children)) {}

void MultiEventEmitter::Emit(proto::TraceEvent ev) {
  if (children_.empty()) return;
  for (size_t i = 0; i + 1 < children_.size(); ++i) {
    children_[i]->Emit(ev);
  }
  children_.back()->Emit(std::move(ev));
}

}  // namespace agentflow
