// tests/unit/persist/file_checkpoint_writer_test.cc
#include "agentflow/persist/file_checkpoint_writer.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "agentflow/persist/checkpoint_reader.h"
#include "checkpoint.pb.h"

namespace agentflow {
namespace {

std::string TempPath(const std::string& tag) {
  auto p = std::filesystem::temp_directory_path() /
           ("p7_" + tag + "_" +
            std::to_string(reinterpret_cast<uintptr_t>(&tag)) + ".ckpt");
  std::filesystem::remove(p);
  std::filesystem::remove(std::filesystem::path(p.string() + ".tmp"));
  return p.string();
}

TEST(FileCheckpointWriterTest, WriteAndReadRoundTrip) {
  auto path = TempPath("rt");

  proto::Checkpoint cp;
  cp.set_state_bytes("hello\x00world", 11);
  cp.set_state_type("agentflow.test.TestState");
  cp.add_completed_nodes("a");
  cp.add_completed_nodes("b");
  cp.set_unix_micros(123456789);
  cp.set_schema_version(1);

  FileCheckpointWriter writer(path);
  ASSERT_TRUE(writer.Write(cp).ok());

  auto read = ReadCheckpointFromFile(path);
  ASSERT_TRUE(read.ok()) << read.status();
  EXPECT_EQ(read->state_bytes(), std::string("hello\x00world", 11));
  EXPECT_EQ(read->state_type(), "agentflow.test.TestState");
  ASSERT_EQ(read->completed_nodes_size(), 2);
  EXPECT_EQ(read->completed_nodes(0), "a");
  EXPECT_EQ(read->completed_nodes(1), "b");
  EXPECT_EQ(read->unix_micros(), 123456789);
  EXPECT_EQ(read->schema_version(), 1u);

  std::filesystem::remove(path);
}

TEST(FileCheckpointWriterTest, WriteOverwritesPriorCheckpoint) {
  auto path = TempPath("overwrite");
  FileCheckpointWriter writer(path);

  proto::Checkpoint a;
  a.add_completed_nodes("first");
  ASSERT_TRUE(writer.Write(a).ok());

  proto::Checkpoint b;
  b.add_completed_nodes("second");
  ASSERT_TRUE(writer.Write(b).ok());

  auto read = ReadCheckpointFromFile(path);
  ASSERT_TRUE(read.ok());
  ASSERT_EQ(read->completed_nodes_size(), 1);
  EXPECT_EQ(read->completed_nodes(0), "second");

  // The .tmp sibling must be cleaned up by the rename — never leaves
  // an orphan on success.
  EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
  std::filesystem::remove(path);
}

TEST(CheckpointReaderTest, MissingFileReturnsNotFound) {
  auto path = TempPath("missing");
  auto result = ReadCheckpointFromFile(path);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

TEST(CheckpointReaderTest, CorruptFileReturnsDataLoss) {
  auto path = TempPath("corrupt");
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    // Bytes that aren't a valid Checkpoint serialization. Protobuf parsing
    // is lenient and accepts most byte sequences, so use clearly malformed
    // wire format — a tag with field=0 (invalid).
    const unsigned char garbage[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    std::fwrite(garbage, 1, sizeof(garbage), f);
    std::fclose(f);
  }
  auto result = ReadCheckpointFromFile(path);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kDataLoss);
  std::filesystem::remove(path);
}

TEST(CheckpointReaderTest, FutureSchemaVersionReturnsFailedPrecondition) {
  auto path = TempPath("future_schema");

  proto::Checkpoint cp;
  cp.set_schema_version(kCurrentCheckpointSchemaVersion + 1);
  FileCheckpointWriter writer(path);
  ASSERT_TRUE(writer.Write(cp).ok());

  auto result = ReadCheckpointFromFile(path);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);
  std::filesystem::remove(path);
}

}  // namespace
}  // namespace agentflow
