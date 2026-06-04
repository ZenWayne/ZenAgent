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
  [[nodiscard]] static State From(ProtoT msg) {
    State s;
    s.msg_ = std::make_unique<ProtoT>(std::move(msg));
    return s;
  }

  template <typename ProtoT>
  [[nodiscard]] static State Empty() {
    State s;
    s.msg_ = std::make_unique<ProtoT>();
    return s;
  }

  // Wraps an already-constructed protobuf message whose concrete type is not
  // known at compile time. This is the entry point for the dynamic-schema path:
  // a DynamicMessage built from a runtime-loaded descriptor (see
  // agentflow/dynamic/SchemaRegistry) is handed in as a base-class pointer.
  [[nodiscard]] static State FromMessage(
      std::unique_ptr<google::protobuf::Message> msg) {
    State s;
    s.msg_ = std::move(msg);
    return s;
  }

  template <typename ProtoT>
  [[nodiscard]] const ProtoT& As() const {
    const auto* typed = dynamic_cast<const ProtoT*>(msg_.get());
    if (!typed) {
      throw AgentflowError(
          "State::As<>: type mismatch (have " +
          std::string(msg_ ? std::string(msg_->GetTypeName()) : "null") + ")");
    }
    return *typed;
  }

  template <typename ProtoT>
  [[nodiscard]] ProtoT& Mutable() {
    auto* typed = dynamic_cast<ProtoT*>(msg_.get());
    if (!typed) {
      throw AgentflowError(
          "State::Mutable<>: type mismatch (have " +
          std::string(msg_ ? std::string(msg_->GetTypeName()) : "null") + ")");
    }
    return *typed;
  }

  [[nodiscard]] std::string SerializeAsString() const;
  bool ParseFromString(std::string_view data);

  [[nodiscard]] State Clone() const;

  // True iff this State holds no message (default-constructed or moved-from).
  // Distinct from the static factory State::Empty<T>(), which constructs a
  // State that holds a default-constructed T (and therefore IsEmpty() == false).
  [[nodiscard]] bool IsEmpty() const noexcept { return msg_ == nullptr; }

  // Raw pointer to the underlying protobuf message (for reflection-based access).
  // Returns nullptr if the State is empty. Use only when the concrete type is
  // unknown at compile time (e.g., AgentNode's field-name-based access).
  [[nodiscard]] const google::protobuf::Message* UnsafeMessage() const {
    return msg_.get();
  }

 private:
  std::unique_ptr<google::protobuf::Message> msg_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_STATE_H_
