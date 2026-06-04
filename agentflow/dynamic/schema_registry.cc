// agentflow/dynamic/schema_registry.cc
#include "agentflow/dynamic/schema_registry.h"

#include <string>

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "absl/strings/str_cat.h"

namespace agentflow {

SchemaRegistry::SchemaRegistry() = default;

absl::Status SchemaRegistry::LoadDescriptorSet(std::string_view serialized_fds) {
  google::protobuf::FileDescriptorSet set;
  {
    google::protobuf::io::ArrayInputStream array_in(
        serialized_fds.data(), static_cast<int>(serialized_fds.size()));
    google::protobuf::io::CodedInputStream coded_in(&array_in);
    coded_in.SetRecursionLimit(100);
    coded_in.SetTotalBytesLimit(64 * 1024 * 1024);  // 64 MiB — bound OOM on
                                                     // untrusted descriptor sets
    if (!set.ParseFromCodedStream(&coded_in) ||
        !coded_in.ConsumedEntireMessage()) {
      return absl::InvalidArgumentError(
          "SchemaRegistry: not a valid FileDescriptorSet");
    }
  }
  // Files must be added after their dependencies; protoc and Wire both emit in
  // topological order, so a single forward pass resolves imports.
  for (const auto& file : set.file()) {
    if (pool_.BuildFile(file) == nullptr) {
      return absl::InvalidArgumentError(
          absl::StrCat("SchemaRegistry: failed to build descriptor for '",
                       file.name(), "' (unresolved import or duplicate type)"));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<google::protobuf::Message>>
SchemaRegistry::NewMessage(std::string_view full_type_name) {
  const google::protobuf::Descriptor* desc =
      pool_.FindMessageTypeByName(std::string(full_type_name));
  if (desc == nullptr) {
    return absl::NotFoundError(absl::StrCat(
        "SchemaRegistry: no message type '", full_type_name, "' loaded"));
  }
  // GetPrototype returns a factory-owned immutable prototype; New() gives the
  // caller an owned, mutable DynamicMessage.
  return std::unique_ptr<google::protobuf::Message>(
      factory_.GetPrototype(desc)->New());
}

}  // namespace agentflow
