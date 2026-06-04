// agentflow/jni/schema_registry_jni.cc
#include <jni.h>

#include <string>

#include "agentflow/dynamic/schema_registry.h"

#define JNI_METHOD(NAME) \
  Java_com_google_ai_edge_agentflow_jni_SchemaRegistryJni_##NAME

namespace {

agentflow::SchemaRegistry* AsRegistry(jlong handle) {
  return reinterpret_cast<agentflow::SchemaRegistry*>(handle);
}

void ThrowIllegalState(JNIEnv* env, const std::string& message) {
  jclass cls = env->FindClass("java/lang/IllegalStateException");
  // cls is null only if the JVM is already out of memory (a pending
  // OutOfMemoryError); calling ThrowNew(null, ...) would be undefined, so let
  // that error propagate instead.
  if (cls != nullptr) env->ThrowNew(cls, message.c_str());
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL JNI_METHOD(nativeCreate)(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(new agentflow::SchemaRegistry());
}

// Frees the registry. The caller must not use `handle` (or any message built
// from it) after this call.
JNIEXPORT void JNICALL JNI_METHOD(nativeDestroy)(JNIEnv*, jclass,
                                                 jlong handle) {
  delete AsRegistry(handle);
}

// Loads a serialized FileDescriptorSet. Throws IllegalStateException with the
// absl::Status message on failure.
JNIEXPORT void JNICALL JNI_METHOD(nativeLoadDescriptorSet)(JNIEnv* env, jclass,
                                                           jlong handle,
                                                           jbyteArray fds) {
  jsize len = env->GetArrayLength(fds);
  jbyte* data = env->GetByteArrayElements(fds, nullptr);
  std::string bytes(reinterpret_cast<const char*>(data),
                    static_cast<size_t>(len));
  env->ReleaseByteArrayElements(fds, data, JNI_ABORT);

  absl::Status st = AsRegistry(handle)->LoadDescriptorSet(bytes);
  if (!st.ok()) ThrowIllegalState(env, std::string(st.message()));
}

// Returns true iff `fullTypeName` resolves to a loaded message type.
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeHasType)(JNIEnv* env, jclass,
                                                     jlong handle,
                                                     jstring fullTypeName) {
  const char* chars = env->GetStringUTFChars(fullTypeName, nullptr);
  std::string name(chars);
  env->ReleaseStringUTFChars(fullTypeName, chars);
  return AsRegistry(handle)->NewMessage(name).ok() ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
