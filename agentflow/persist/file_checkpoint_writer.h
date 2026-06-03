// agentflow/persist/file_checkpoint_writer.h
#ifndef AGENTFLOW_PERSIST_FILE_CHECKPOINT_WRITER_H_
#define AGENTFLOW_PERSIST_FILE_CHECKPOINT_WRITER_H_

#include <mutex>
#include <string>

#include "agentflow/persist/checkpoint_writer.h"

namespace agentflow {

// Writes checkpoints as serialized protobuf to `path`. Each Write goes to
// `path + ".tmp"` first, then `std::rename` atomically swaps it over the
// final path — a `kill -9` mid-write leaves either the prior checkpoint or
// no file, never a half-written one.
//
// `path` and `path + ".tmp"` must live on the same filesystem (POSIX rename
// is only atomic within a single FS).
class FileCheckpointWriter : public CheckpointWriter {
 public:
  explicit FileCheckpointWriter(std::string path);
  absl::Status Write(const proto::Checkpoint& cp) override;

 private:
  std::string path_;
  std::mutex mu_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_PERSIST_FILE_CHECKPOINT_WRITER_H_
