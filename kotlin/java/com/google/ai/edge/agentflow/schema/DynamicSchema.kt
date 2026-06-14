package com.google.ai.edge.agentflow.schema

import com.google.ai.edge.agentflow.jni.SchemaRegistryJni

/** Owns a native SchemaRegistry. Close it to free native memory. */
class DynamicSchema : AutoCloseable {
  // 0 is the closed sentinel; nativeCreate() returns a non-null pointer (it
  // would throw std::bad_alloc before returning null).
  private var handle = SchemaRegistryJni.nativeCreate()
  private val compiler = ProtoSchemaCompiler()

  /** Compile `.proto` text and load it into the native registry. */
  fun load(protoFiles: Map<String, String>) {
    check(handle != 0L) { "DynamicSchema is closed" }
    SchemaRegistryJni.nativeLoadDescriptorSet(handle, compiler.compile(protoFiles))
  }

  fun hasType(fullTypeName: String): Boolean {
    check(handle != 0L) { "DynamicSchema is closed" }
    return SchemaRegistryJni.nativeHasType(handle, fullTypeName)
  }

  // Idempotent: guards against a double nativeDestroy (a C++ double-free).
  override fun close() {
    val h = handle
    if (h != 0L) {
      handle = 0L
      SchemaRegistryJni.nativeDestroy(h)
    }
  }
}
