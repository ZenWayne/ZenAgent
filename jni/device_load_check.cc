// jni/device_load_check.cc
//
// Proves libagentflow_jni.so actually loads on a real Android device and
// exports the entry points the Kotlin layer binds to.
//
// A clean cross-compile proves none of this. Android's loader rejects a .so
// carrying text relocations, and an unresolved symbol only surfaces at dlopen
// time — RTLD_NOW below forces every relocation up front rather than letting
// a missing symbol lie dormant until the first call. Checking the exports
// matters just as much: a .so that loads but exports nothing usable passes
// every static check and still leaves the Kotlin layer with nothing to bind.
//
// Usage:
//   bazel build --config=android_arm64 //jni:libagentflow_jni.so
//   bazel build --config=android_arm64_test //jni:device_load_check
//   adb push bazel-bin/jni/libagentflow_jni.so                /data/local/tmp/afjni/
//   adb push third_party/litert_lm/lib/arm64-v8a/libkissfft-float.so.131 \
//            /data/local/tmp/afjni/libkissfft-float.so
//   adb push bazel-bin/jni/device_load_check                   /data/local/tmp/afjni/
//   adb shell 'cd /data/local/tmp/afjni && chmod +x device_load_check && \
//              LD_LIBRARY_PATH=. ./device_load_check ./libagentflow_jni.so'
//
// Exit code is 0 only when the library loads AND every entry point resolves.
//
// Note: loading the library emits an ERROR-level line from LiteRT-LM's own
// static initialisers —
//   engine_factory.h] Failed to register engine: ALREADY_EXISTS
// That comes from the prebuilt LiteRT archive's engine registration, not from
// agentflow, and the first registration succeeds. It is noise, not a failure.

#include <dlfcn.h>

#include <cstdio>

namespace {

// Taken from `llvm-nm -D --defined-only libagentflow_jni.so`, not guessed.
// If the Kotlin bridge gains a native method, add it here — that is the point
// of the list.
constexpr const char* kEntryPoints[] = {
    "Java_agentflow_jni_NativeBridge_runAgent",
    "Java_agentflow_jni_NativeBridge_runJsonWorkflow",
    "Java_agentflow_jni_NativeBridge_runJsonWorkflowStreaming",
    "Java_agentflow_jni_NativeBridge_nativeNewCancel",
    "Java_agentflow_jni_NativeBridge_nativeCancel",
    "Java_agentflow_jni_NativeBridge_nativeFreeCancel",
};

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "./libagentflow_jni.so";

  void* handle = dlopen(path, RTLD_NOW);
  if (handle == nullptr) {
    std::printf("DLOPEN FAILED: %s\n", dlerror());
    return 1;
  }
  std::printf("DLOPEN OK: %s\n", path);

  constexpr unsigned kCount =
      sizeof(kEntryPoints) / sizeof(kEntryPoints[0]);
  unsigned found = 0;
  for (unsigned i = 0; i < kCount; ++i) {
    dlerror();  // clear any stale error before the lookup
    void* symbol = dlsym(handle, kEntryPoints[i]);
    std::printf("  %-58s %s\n", kEntryPoints[i],
                symbol != nullptr ? "FOUND" : "ABSENT");
    if (symbol != nullptr) ++found;
  }

  std::printf("%s: %u/%u entry points resolved\n",
              found == kCount ? "SYMBOLS OK" : "SYMBOLS INCOMPLETE", found,
              kCount);
  dlclose(handle);
  return found == kCount ? 0 : 2;
}
