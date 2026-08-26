// agentflow/persist/checkpoint_writer.h
#ifndef AGENTFLOW_PERSIST_CHECKPOINT_WRITER_H_
#define AGENTFLOW_PERSIST_CHECKPOINT_WRITER_H_

#include "absl/status/status.h"

#include "checkpoint.pb.h"

namespace agentflow {

// Schema version stamped into every checkpoint we write. Bump if the wire
// format ever changes incompatibly. Resume rejects checkpoints with a
// schema_version greater than this constant.
inline constexpr uint32_t kCurrentCheckpointSchemaVersion = 1;

// Sink for serialized Runner checkpoints. Implementations must be safe to
// call from the Runner's io_context thread; the Runner does not retry on
// failure (write errors are logged + the graph continues).
class CheckpointWriter {
 public:
  virtual ~CheckpointWriter() = default;
  virtual absl::Status Write(const proto::Checkpoint& cp) = 0;
};

}  // namespace agentflow

#endif  // AGENTFLOW_PERSIST_CHECKPOINT_WRITER_H_
