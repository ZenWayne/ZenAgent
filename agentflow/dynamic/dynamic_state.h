// agentflow/dynamic/dynamic_state.h
#ifndef AGENTFLOW_DYNAMIC_DYNAMIC_STATE_H_
#define AGENTFLOW_DYNAMIC_DYNAMIC_STATE_H_

#include "absl/status/statusor.h"

#include "agentflow/core/state.h"
#include "agentflow/dynamic/schema_registry.h"

namespace agentflow {

namespace proto { class Checkpoint; }  // proto/checkpoint.proto

// Builds an empty State of the checkpoint's declared type, ready to hand to
// Runner::Resume. The State holds a DynamicMessage backed by `reg`'s pool, so
// `reg` MUST outlive the returned State. Returns NotFound if cp.state_type()
// has not been loaded into `reg`.
//
// `reg` is non-const: DynamicMessageFactory::GetPrototype (via NewMessage) may
// lazily populate factory state.
absl::StatusOr<State> MakeResumeTarget(SchemaRegistry& reg,
                                       const proto::Checkpoint& cp);

}  // namespace agentflow

#endif  // AGENTFLOW_DYNAMIC_DYNAMIC_STATE_H_
