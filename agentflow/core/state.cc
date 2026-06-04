// agentflow/core/state.cc
#include "agentflow/core/state.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "absl/status/status.h"

namespace agentflow {

absl::Status ParseBoundedIntoMessage(google::protobuf::Message& msg,
                                     std::string_view bytes, int max_depth,
                                     int max_bytes) {
  // Reject early if the input already exceeds the byte budget. This also keeps
  // the static_cast<int> below well-defined: a >2 GiB string would otherwise
  // truncate to a negative size.
  if (bytes.size() > static_cast<size_t>(max_bytes)) {
    return absl::InvalidArgumentError(
        "ParseBoundedIntoMessage: input exceeds max_bytes");
  }
  google::protobuf::io::ArrayInputStream array_in(
      bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedInputStream coded_in(&array_in);
  coded_in.SetRecursionLimit(max_depth);
  coded_in.SetTotalBytesLimit(max_bytes);
  msg.Clear();
  if (!msg.ParseFromCodedStream(&coded_in) ||
      !coded_in.ConsumedEntireMessage()) {
    return absl::InvalidArgumentError(
        "ParseBoundedIntoMessage: malformed message or limit exceeded");
  }
  return absl::OkStatus();
}

absl::Status State::ParseFromStringBounded(std::string_view data, int max_depth,
                                           int max_bytes) {
  if (!msg_) {
    return absl::FailedPreconditionError(
        "State::ParseFromStringBounded: no message");
  }
  return ParseBoundedIntoMessage(*msg_, data, max_depth, max_bytes);
}

std::string State::SerializeAsString() const {
  if (!msg_) return {};
  std::string out;
  msg_->SerializeToString(&out);
  return out;
}

bool State::ParseFromString(std::string_view data) {
  if (!msg_) return false;
  return msg_->ParseFromArray(data.data(), static_cast<int>(data.size()));
}

State State::Clone() const {
  State out;
  if (msg_) {
    out.msg_.reset(msg_->New());
    out.msg_->CopyFrom(*msg_);
  }
  return out;
}

}  // namespace agentflow
