// agentflow/dynamic/dynamic_state.cc
#include "agentflow/dynamic/dynamic_state.h"

#include <utility>

#include "checkpoint.pb.h"

namespace agentflow {

absl::StatusOr<State> MakeResumeTarget(SchemaRegistry& reg,
                                       const proto::Checkpoint& cp) {
  auto msg = reg.NewMessage(cp.state_type());
  if (!msg.ok()) return msg.status();
  return State::FromMessage(std::move(*msg));
}

}  // namespace agentflow
