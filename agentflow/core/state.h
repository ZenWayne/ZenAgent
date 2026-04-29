// agentflow/core/state.h
#ifndef AGENTFLOW_CORE_STATE_H_
#define AGENTFLOW_CORE_STATE_H_

#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>

#include <google/protobuf/message.h>

#include "agentflow/core/errors.h"

namespace agentflow {

// Type-erased holder for a single protobuf message representing graph state.
//
// Concurrency: a State instance is not thread-safe. The runner gives each node
// an independent State instance during execution (Clone() at fan-out, merge at
// fan-in).
class State {
 public:
  State() = default;

  template <typename ProtoT>
  static State From(ProtoT msg) {
    State s;
    s.msg_ = std::make_unique<ProtoT>(std::move(msg));
    return s;
  }

  template <typename ProtoT>
  static State Empty() {
    State s;
    s.msg_ = std::make_unique<ProtoT>();
    return s;
  }

  template <typename ProtoT>
  const ProtoT& As() const {
    const auto* typed = dynamic_cast<const ProtoT*>(msg_.get());
    if (!typed) {
      throw AgentflowError(
          std::string("State::As<>: type mismatch (have ") +
          (msg_ ? msg_->GetTypeName() : "null") + ")");
    }
    return *typed;
  }

  template <typename ProtoT>
  ProtoT& Mutable() {
    auto* typed = dynamic_cast<ProtoT*>(msg_.get());
    if (!typed) {
      throw AgentflowError(
          std::string("State::Mutable<>: type mismatch (have ") +
          (msg_ ? msg_->GetTypeName() : "null") + ")");
    }
    return *typed;
  }

  std::string SerializeAsString() const;
  bool ParseFromString(std::string_view data);

  State Clone() const;

  bool Empty() const noexcept { return msg_ == nullptr; }

 private:
  std::unique_ptr<google::protobuf::Message> msg_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_STATE_H_
