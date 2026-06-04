// agentflow/dynamic/dynamic_state.h
#ifndef AGENTFLOW_DYNAMIC_DYNAMIC_STATE_H_
#define AGENTFLOW_DYNAMIC_DYNAMIC_STATE_H_

#include <string_view>

#include <google/protobuf/message.h>

#include "absl/status/status.h"
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

// Default bounds for parsing untrusted serialized State.
inline constexpr int kDefaultMaxParseDepth = 100;
inline constexpr int kDefaultMaxParseBytes = 64 * 1024 * 1024;  // 64 MiB

// Parses `bytes` into `msg` with bounded recursion depth and total size, and
// requires the entire input to be consumed. Use for untrusted State payloads
// (e.g. a checkpoint from disk or a third party). Returns InvalidArgument on a
// malformed message, a limit breach, or trailing garbage.
absl::Status ParseStateBytes(google::protobuf::Message& msg,
                             std::string_view bytes,
                             int max_depth = kDefaultMaxParseDepth,
                             int max_bytes = kDefaultMaxParseBytes);

}  // namespace agentflow

#endif  // AGENTFLOW_DYNAMIC_DYNAMIC_STATE_H_
