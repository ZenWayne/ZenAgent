// agentflow/persist/file_checkpoint_writer.cc
#include "agentflow/persist/file_checkpoint_writer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

#include <absl/strings/str_cat.h>

namespace agentflow {

FileCheckpointWriter::FileCheckpointWriter(std::string path)
    : path_(std::move(path)) {}

absl::Status FileCheckpointWriter::Write(const proto::Checkpoint& cp) {
  std::string bytes;
  if (!cp.SerializeToString(&bytes)) {
    return absl::InternalError("Checkpoint::SerializeToString failed");
  }

  std::lock_guard<std::mutex> lk(mu_);
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return absl::UnavailableError(
          absl::StrCat("open ", tmp, " for write: ", std::strerror(errno)));
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      return absl::DataLossError(
          absl::StrCat("write to ", tmp, ": ", std::strerror(errno)));
    }
  }  // closes the file before rename — required on Windows; harmless on POSIX
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    return absl::UnavailableError(
        absl::StrCat("rename ", tmp, " -> ", path_, ": ",
                     std::strerror(errno)));
  }
  return absl::OkStatus();
}

}  // namespace agentflow
