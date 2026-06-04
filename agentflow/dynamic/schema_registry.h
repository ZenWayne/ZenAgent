// agentflow/dynamic/schema_registry.h
#ifndef AGENTFLOW_DYNAMIC_SCHEMA_REGISTRY_H_
#define AGENTFLOW_DYNAMIC_SCHEMA_REGISTRY_H_

#include <memory>
#include <string>
#include <string_view>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace agentflow {

// Owns a DescriptorPool and DynamicMessageFactory that are populated at runtime
// from serialized protobuf descriptors (a google.protobuf.FileDescriptorSet,
// e.g. produced JVM-side by Wire's SchemaEncoder and shipped over JNI).
//
// This is the C++ end of the dynamic-schema path: the app receives a .proto
// schema it did not know at build time, compiles it to a descriptor set off the
// hot path, hands the bytes here, then builds State messages by type name with
// no generated C++ code.
//
// Lifetime: every Message returned by NewMessage() borrows descriptors and the
// prototype factory owned by this registry. The registry MUST outlive all
// messages it produced.
//
// Thread-safety: not thread-safe. Guard with an external mutex if shared.
class SchemaRegistry {
 public:
  SchemaRegistry();

  // Parses `serialized_fds` as a FileDescriptorSet and adds every file to the
  // pool. Files must appear after their dependencies (topological order), as
  // protoc and Wire both emit. Returns InvalidArgument on a malformed set or a
  // file whose imports/types cannot be resolved.
  absl::Status LoadDescriptorSet(std::string_view serialized_fds);

  // Builds an empty, mutable message for the fully-qualified type name (e.g.
  // "dyntest.Person"). The caller drives it via reflection or hands it to
  // State::FromMessage. Returns NotFound if no such type has been loaded.
  absl::StatusOr<std::unique_ptr<google::protobuf::Message>> NewMessage(
      std::string_view full_type_name);

 private:
  google::protobuf::DescriptorPool pool_;
  google::protobuf::DynamicMessageFactory factory_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_DYNAMIC_SCHEMA_REGISTRY_H_
