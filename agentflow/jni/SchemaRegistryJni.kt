package com.google.ai.edge.agentflow.jni

import java.io.File

/** 1:1 binding to agentflow/jni/schema_registry_jni.cc. */
object SchemaRegistryJni {
  private const val LIB_NAME = "agentflow_jni"

  init {
    loadNative()
  }

  private fun loadNative() {
    // Prefer an absolute path injected via -Dagentflow.jni.so.path (useful for
    // Bazel tests that pass a custom sandbox path).
    val explicitPath = System.getProperty("agentflow.jni.so.path")
    if (explicitPath != null) {
      System.load(explicitPath)
      return
    }

    // In Bazel tests the .so is declared in `data`; it lands in the runfiles
    // tree at $JAVA_RUNFILES/_main/agentflow/jni/lib<name>.so. "_main" is the
    // runfiles dir of the ROOT module only (bzlmod). If this library is ever
    // consumed as a bazel_dep by another module, its .so lands under a
    // different canonical name; this probe then misses and we fall through to
    // System.loadLibrary below (still correct).
    val javaRunfiles = System.getenv("JAVA_RUNFILES")
    if (javaRunfiles != null) {
      val runfilesSo = File(javaRunfiles, "_main/agentflow/jni/lib${LIB_NAME}.so")
      if (runfilesSo.exists()) {
        System.load(runfilesSo.absolutePath)
        return
      }
    }

    // Fallback: rely on java.library.path / LD_LIBRARY_PATH (Android, desktop).
    System.loadLibrary(LIB_NAME)
  }

  external fun nativeCreate(): Long
  external fun nativeDestroy(handle: Long)
  /** Throws IllegalStateException with the absl::Status message on failure. */
  external fun nativeLoadDescriptorSet(handle: Long, fds: ByteArray)
  external fun nativeHasType(handle: Long, fullTypeName: String): Boolean
}
