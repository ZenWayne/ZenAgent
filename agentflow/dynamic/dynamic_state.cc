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

absl::Status ParseStateBytes(google::protobuf::Message& msg,
                             std::string_view bytes, int max_depth,
                             int max_bytes) {
  return ParseBoundedIntoMessage(msg, bytes, max_depth, max_bytes);
}

}  // namespace agentflow
