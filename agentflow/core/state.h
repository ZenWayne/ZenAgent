// agentflow/core/state.h
#ifndef AGENTFLOW_CORE_STATE_H_
#define AGENTFLOW_CORE_STATE_H_

#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <variant>

#include <google/protobuf/message.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/errors.h"

namespace google {
namespace protobuf {
class DescriptorPool;
class DynamicMessageFactory;
}  // namespace protobuf
}  // namespace google

namespace agentflow {

// Type-erased holder for graph state. Three backing tiers:
//   tier 1 (Proto)        — generated proto message, statically typed.
//   tier 2 (Json)         — nlohmann::ordered_json backed by a fields decl.
//   tier 3 (ProtoDynamic) — runtime Message constructed from a caller-supplied
//                           DescriptorPool via DynamicMessageFactory; the pool
//                           is kept alive on the State.
//
// Concurrency: a State instance is not thread-safe. The runner gives each node
// an independent State instance during execution (Clone() at fan-out, merge at
// fan-in).
class State {
 public:
  enum class Kind { Proto, Json, ProtoDynamic };

  State() = default;

  template <typename ProtoT>
  [[nodiscard]] static State From(ProtoT msg) {
    State s;
    s.backing_ = std::unique_ptr<google::protobuf::Message>(
        std::make_unique<ProtoT>(std::move(msg)));
    return s;
  }

  template <typename ProtoT>
  [[nodiscard]] static State Empty() {
    State s;
    s.backing_ = std::unique_ptr<google::protobuf::Message>(
        std::make_unique<ProtoT>());
    return s;
  }

  // Wraps an already-constructed protobuf message whose concrete type is not
  // known at compile time. This is the entry point for the dynamic-schema path:
  // a DynamicMessage built from a runtime-loaded descriptor (see
  // agentflow/dynamic/SchemaRegistry) is handed in as a base-class pointer.
  //
  // Unlike FromDynamicProto(), this does NOT install a pool keepalive: the
  // message borrows its descriptors/prototype from whatever built it (e.g. a
  // SchemaRegistry), which the caller guarantees outlives this State. Prefer
  // FromDynamicProto() when the State itself should own the pool's lifetime.
  [[nodiscard]] static State FromMessage(
      std::unique_ptr<google::protobuf::Message> msg) {
    State s;
    s.backing_ = std::move(msg);
    return s;
  }

  // Build a JSON-backed state from a fields declaration (typically the
  // WorkflowSpec.state.fields entry). Each entry's "default" is used if
  // present; otherwise initialized by "type" ("string"->"", "integer"->0,
  // "number"->0.0, "boolean"->false, "array"->[], "object"->{}, else null).
  [[nodiscard]] static State FromJson(
      const nlohmann::ordered_json& fields_decl);

  // Build a dynamic-proto-backed state by resolving `message_type` against
  // the supplied DescriptorPool, then constructing a Message via
  // DynamicMessageFactory. The pool is held by the State (and propagated by
  // Clone()) so that the Message and any reflection access remain valid for
  // the State's lifetime. Returns an empty State if `pool` is null or the
  // descriptor cannot be found.
  [[nodiscard]] static State FromDynamicProto(
      std::shared_ptr<google::protobuf::DescriptorPool> pool,
      std::string_view message_type);

  // Returns the active variant arm. A default-constructed State returns
  // Kind::Proto with a null underlying pointer; callers should check
  // IsEmpty() first before relying on the arm's contents. A proto-arm State
  // that was constructed via FromDynamicProto() reports Kind::ProtoDynamic
  // (the pool keepalive distinguishes it).
  [[nodiscard]] Kind kind() const noexcept;

  template <typename ProtoT>
  [[nodiscard]] const ProtoT& As() const {
    const auto* slot =
        std::get_if<std::unique_ptr<google::protobuf::Message>>(&backing_);
    const google::protobuf::Message* raw = slot ? slot->get() : nullptr;
    const auto* typed = dynamic_cast<const ProtoT*>(raw);
    if (!typed) {
      throw AgentflowError(
          "State::As<>: type mismatch (have " +
          std::string(raw ? std::string(raw->GetTypeName()) : "null") + ")");
    }
    return *typed;
  }

  template <typename ProtoT>
  [[nodiscard]] ProtoT& Mutable() {
    auto* slot =
        std::get_if<std::unique_ptr<google::protobuf::Message>>(&backing_);
    google::protobuf::Message* raw = slot ? slot->get() : nullptr;
    auto* typed = dynamic_cast<ProtoT*>(raw);
    if (!typed) {
      throw AgentflowError(
          "State::Mutable<>: type mismatch (have " +
          std::string(raw ? std::string(raw->GetTypeName()) : "null") + ")");
    }
    return *typed;
  }

  [[nodiscard]] std::string SerializeAsString() const;
  bool ParseFromString(std::string_view data);

  [[nodiscard]] State Clone() const;

  // True iff this State holds no message (default-constructed, moved-from, or
  // a variant arm holding a null unique_ptr).
  [[nodiscard]] bool IsEmpty() const noexcept;

  // Raw pointer to the underlying protobuf message (for reflection-based
  // access). Returns nullptr if the State is empty or holds a JSON arm.
  [[nodiscard]] const google::protobuf::Message* UnsafeMessage() const;

  // Free-function helpers need to look into the variant.
  friend std::string ReadStringField(const State& s, std::string_view path);
  friend void WriteStringField(State& s, std::string_view path,
                               std::string_view value);
  friend const nlohmann::ordered_json* AsJson(const State& s);
  friend nlohmann::ordered_json* MutableJson(State& s);

 private:
  // Bundle of pool + factory bound to that pool. Lifetime is shared because
  // the dynamic Message instance (in `backing_`) keeps non-owning pointers
  // into the factory (vtable/prototype) and the pool (descriptors). Both
  // must outlive the message.
  struct DynamicEnv {
    std::shared_ptr<google::protobuf::DescriptorPool> pool;
    std::unique_ptr<google::protobuf::DynamicMessageFactory> factory;
    ~DynamicEnv();
  };

  // NOTE: declaration order matters for destruction. The dynamic Message in
  // `backing_` holds non-owning pointers into the factory (vtable/prototype)
  // and pool (descriptors) owned by `pool_`. C++ destroys members in reverse
  // declaration order, so `pool_` must be declared FIRST and `backing_`
  // SECOND so that `backing_` (the Message) is torn down before `pool_`
  // releases the factory + pool.

  // Non-null iff this State was built via FromDynamicProto(); keeps the
  // pool + factory (and thus the descriptor / dynamic-message vtable) alive
  // so the Message remains valid past the caller's nominal pool scope.
  std::shared_ptr<DynamicEnv> pool_;

  std::variant<std::unique_ptr<google::protobuf::Message>,
               std::unique_ptr<nlohmann::ordered_json>>
      backing_;
};

// Dotted-path string read/write.
//
// Proto arm: the path is treated as a single top-level field name; only
// TYPE_STRING fields are honored. Read on a missing/non-string field returns
// "". Write on a missing/non-string field silently noops.
//
// JSON arm: the path is split on '.'. Read returns "" on any missing or
// non-object intermediate; if the resolved leaf is a string, returns the raw
// value, otherwise returns its JSON dump. Write auto-creates intermediate
// objects as needed.
std::string ReadStringField(const State& s, std::string_view path);
void WriteStringField(State& s, std::string_view path,
                      std::string_view value);

// Direct access to the JSON arm. Returns nullptr if the state is proto-backed.
const nlohmann::ordered_json* AsJson(const State& s);
nlohmann::ordered_json* MutableJson(State& s);

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_STATE_H_
