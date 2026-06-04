// agentflow/dynamic/dynamic_state.cc
#include "agentflow/dynamic/dynamic_state.h"

#include <utility>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

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
  google::protobuf::io::ArrayInputStream array_in(
      bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedInputStream coded_in(&array_in);
  coded_in.SetRecursionLimit(max_depth);
  coded_in.SetTotalBytesLimit(max_bytes);
  msg.Clear();
  if (!msg.ParseFromCodedStream(&coded_in) ||
      !coded_in.ConsumedEntireMessage()) {
    return absl::InvalidArgumentError(
        "ParseStateBytes: malformed message or limit exceeded");
  }
  return absl::OkStatus();
}

}  // namespace agentflow
