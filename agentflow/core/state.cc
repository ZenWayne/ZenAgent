// agentflow/core/state.cc
#include "agentflow/core/state.h"

namespace agentflow {

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
