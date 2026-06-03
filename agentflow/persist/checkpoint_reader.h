// agentflow/persist/checkpoint_reader.h
#ifndef AGENTFLOW_PERSIST_CHECKPOINT_READER_H_
#define AGENTFLOW_PERSIST_CHECKPOINT_READER_H_

#include <string>

#include <absl/status/statusor.h>

#include "checkpoint.pb.h"

namespace agentflow {

// Reads a serialized Checkpoint from `path`. Returns:
//   NotFoundError      — file does not exist.
//   DataLossError      — file exists but can't be parsed.
//   FailedPrecondition — schema_version > kCurrentCheckpointSchemaVersion.
absl::StatusOr<proto::Checkpoint> ReadCheckpointFromFile(
    const std::string& path);

}  // namespace agentflow

#endif  // AGENTFLOW_PERSIST_CHECKPOINT_READER_H_
