// agentflow/persist/checkpoint_reader.cc
#include "agentflow/persist/checkpoint_reader.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "absl/strings/str_cat.h"

#include "agentflow/persist/checkpoint_writer.h"  // for kCurrentCheckpointSchemaVersion

namespace agentflow {

absl::StatusOr<proto::Checkpoint> ReadCheckpointFromFile(
    const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat("open ", path, " for read: ", std::strerror(errno)));
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string bytes = ss.str();

  proto::Checkpoint cp;
  if (!cp.ParseFromString(bytes)) {
    return absl::DataLossError(
        absl::StrCat("parse Checkpoint from ", path, " failed"));
  }
  if (cp.schema_version() > kCurrentCheckpointSchemaVersion) {
    return absl::FailedPreconditionError(
        absl::StrCat("checkpoint schema_version=", cp.schema_version(),
                     " is newer than supported ",
                     kCurrentCheckpointSchemaVersion));
  }
  return cp;
}

}  // namespace agentflow
