package com.google.ai.edge.agentflow.schema

import com.google.protobuf.DescriptorProtos.FileDescriptorSet
import com.google.protobuf.Descriptors
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ProtoSchemaCompilerTest {
  private val personProto = """
    syntax = "proto3";
    package dyntest;
    message Person {
      string name = 1;
      int32 age = 2;
    }
  """.trimIndent()

  @Test fun `compiles proto text to a parseable FileDescriptorSet`() {
    val bytes = ProtoSchemaCompiler()
      .compile(mapOf("dyntest/person.proto" to personProto))

    val set = FileDescriptorSet.parseFrom(bytes)
    assertEquals(1, set.fileCount)

    val fd = Descriptors.FileDescriptor.buildFrom(
      set.getFile(0), emptyArray<Descriptors.FileDescriptor>())
    val person = fd.findMessageTypeByName("Person")
    assertNotNull(person)
    assertEquals("dyntest.Person", person.fullName)
    assertEquals(
      Descriptors.FieldDescriptor.Type.STRING,
      person.findFieldByName("name").type)
    assertEquals(
      Descriptors.FieldDescriptor.Type.INT32,
      person.findFieldByName("age").type)
  }

  @Test fun `resolves a local import across two files`() {
    val common = """
      syntax = "proto3";
      package dyntest;
      message Addr { string city = 1; }
    """.trimIndent()
    val person = """
      syntax = "proto3";
      package dyntest;
      import "dyntest/addr.proto";
      message Person { string name = 1; Addr addr = 2; }
    """.trimIndent()

    val bytes = ProtoSchemaCompiler().compile(
      mapOf("dyntest/addr.proto" to common, "dyntest/person.proto" to person))
    val set = FileDescriptorSet.parseFrom(bytes)
    assertEquals(2, set.fileCount)

    val byName = set.fileList.associateBy { it.name }
    val addrFd = Descriptors.FileDescriptor.buildFrom(
      byName["dyntest/addr.proto"]!!, emptyArray())
    val personFd = Descriptors.FileDescriptor.buildFrom(
      byName["dyntest/person.proto"]!!, arrayOf(addrFd))
    assertTrue(personFd.findMessageTypeByName("Person")
      .findFieldByName("addr").messageType.fullName == "dyntest.Addr")
  }

  // The output must be in topological order (imports before importers) no
  // matter how the caller ordered the input map, because the C++ consumer
  // (DescriptorPool::BuildFile) does a single forward pass. Here the dependent
  // "person" is inserted BEFORE its dependency "addr".
  @Test fun `emits files in dependency order regardless of input order`() {
    val common = """
      syntax = "proto3";
      package dyntest;
      message Addr { string city = 1; }
    """.trimIndent()
    val person = """
      syntax = "proto3";
      package dyntest;
      import "dyntest/addr.proto";
      message Person { string name = 1; Addr addr = 2; }
    """.trimIndent()

    val bytes = ProtoSchemaCompiler().compile(
      linkedMapOf("dyntest/person.proto" to person,
                  "dyntest/addr.proto" to common))
    val set = FileDescriptorSet.parseFrom(bytes)

    // Mirror the C++ consumer: build each file in set order, requiring every
    // dependency to already be built. Fails if the emitted order is wrong.
    val built = LinkedHashMap<String, Descriptors.FileDescriptor>()
    for (f in set.fileList) {
      val deps = f.dependencyList.map {
        built[it] ?: error("dependency $it appears after dependent ${f.name}")
      }
      built[f.name] = Descriptors.FileDescriptor.buildFrom(f, deps.toTypedArray())
    }
    assertNotNull(built["dyntest/person.proto"])
  }

  // Exercises the multi-byte varint length-prefix path: a FileDescriptorProto
  // with many long field names exceeds 127 bytes, so writeVarint must emit a
  // continuation byte. protobuf-java parsing it back proves the prefix is right.
  @Test fun `handles descriptor larger than 127 bytes`() {
    val big = """
      syntax = "proto3";
      package dyntest;
      message BigMessage {
        string field_with_a_fairly_long_name_one   = 1;
        string field_with_a_fairly_long_name_two   = 2;
        string field_with_a_fairly_long_name_three = 3;
        int32  field_with_a_fairly_long_name_four  = 4;
        int32  field_with_a_fairly_long_name_five  = 5;
      }
    """.trimIndent()

    val bytes = ProtoSchemaCompiler().compile(mapOf("dyntest/big.proto" to big))
    val set = FileDescriptorSet.parseFrom(bytes)
    assertEquals(1, set.fileCount)
    assertNotNull(
      Descriptors.FileDescriptor.buildFrom(set.getFile(0), emptyArray())
        .findMessageTypeByName("BigMessage"))
  }
}
