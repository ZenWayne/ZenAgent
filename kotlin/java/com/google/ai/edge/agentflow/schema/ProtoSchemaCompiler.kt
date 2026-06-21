package com.google.ai.edge.agentflow.schema

import com.squareup.wire.schema.Location
import com.squareup.wire.schema.SchemaLoader
import com.squareup.wire.schema.internal.SchemaEncoder
import okio.Buffer
import okio.ByteString
import okio.Path.Companion.toPath
import okio.fakefilesystem.FakeFileSystem

/**
 * Compiles in-memory `.proto` source files into a serialized
 * `google.protobuf.FileDescriptorSet`, for a C++ DescriptorPool.
 *
 * Uses Wire's SchemaLoader (parse) + SchemaEncoder (encode). SchemaEncoder is
 * in Wire's `internal` package: output is protoc-compatible (verified by Wire's
 * SchemaEncoderInteropTest) but the API has no stability guarantee — pin the
 * Wire version; re-run tests on any upgrade.
 */
class ProtoSchemaCompiler {

  /**
   * @param protoFiles map of proto import path -> source text. Input order does
   *   NOT matter: output files are emitted in Wire's dependency-resolved order
   *   (imports before importers), so the C++ DescriptorPool can BuildFile them
   *   in a single forward pass.
   */
  fun compile(protoFiles: Map<String, String>): ByteArray {
    val fs = FakeFileSystem()
    val root = "/src".toPath()
    fs.createDirectories(root)
    for ((path, content) in protoFiles) {
      val full = root / path
      full.parent?.let { fs.createDirectories(it) }
      fs.write(full) { writeUtf8(content) }
    }

    val schema = SchemaLoader(fs).apply {
      initRoots(sourcePath = listOf(Location.get(root.toString())))
    }.loadSchema()

    val encoder = SchemaEncoder(schema)

    // FileDescriptorSet = repeated FileDescriptorProto file = 1.
    // Iterate schema.protoFiles (Wire resolves imports before dependents) and
    // keep only our inputs, dropping Wire's bundled builtins (descriptor.proto,
    // well-known types). This guarantees topological order regardless of how
    // the caller ordered `protoFiles`.
    val out = Buffer()
    for (protoFile in schema.protoFiles) {
      if (protoFile.location.path !in protoFiles) continue
      val fdpBytes: ByteString = encoder.encode(protoFile)
      out.writeByte(0x0A)              // field 1, wiretype LEN (2)
      writeVarint(out, fdpBytes.size)  // length prefix
      out.write(fdpBytes)              // the FileDescriptorProto
    }
    return out.readByteArray()
  }

  private fun writeVarint(buffer: Buffer, value: Int) {
    var v = value
    while (true) {
      if (v and 0x7F.inv() == 0) { buffer.writeByte(v); return }
      buffer.writeByte((v and 0x7F) or 0x80)
      v = v ushr 7
    }
  }
}
