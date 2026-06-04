# Dynamic Schema (B1) — End-to-End Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the agentflow C++ framework run user/third-party `.proto` schemas it never saw at build time — parse `.proto` text JVM-side with Square Wire, ship the compiled descriptor over JNI, and drive graph State as a `DynamicMessage` in C++ — including checkpoint/resume.

**Architecture:** Three subsystems, each independently testable:
1. **C++ integration** — turn the already-built `SchemaRegistry` (descriptor bytes → `DynamicMessage`) into a usable resume/run path, and harden parsing of untrusted bytes.
2. **JNI bridge** — a thin marshalling shim that owns a long-lived `SchemaRegistry` behind an opaque handle; all logic stays in the (unit-tested) C++ layer.
3. **JVM/Kotlin compiler** — Wire `SchemaLoader` parses `.proto` text; `SchemaEncoder` emits per-file `FileDescriptorProto`; we assemble a `FileDescriptorSet` and hand the bytes to JNI.

**Tech Stack:** C++17, protobuf 31.1 (Bazel `git_override`, shared with LiteRT-LM), Abseil, asio, GoogleTest, Bazel 7.4.1; Kotlin + Square Wire (`wire-schema` ≥ 4.8.0), `rules_kotlin`, protobuf-java (test-only validation).

**Prerequisite (DONE — do not redo):** `agentflow/dynamic/schema_registry.{h,cc}` exists with `LoadDescriptorSet` + `NewMessage`, and `State::FromMessage(std::unique_ptr<Message>)` exists in `agentflow/core/state.h`. Both are covered by passing tests (`//tests/unit/dynamic:schema_registry_test`, `//tests/unit/core:state_test`). This plan builds on top.

**Build note:** Bazel/protoc/gh are fronted by the `.ultra_sandbox` wrapper and must be "mapped" (enabled) for the session. All `bazel` commands below need the proxy JVM args:
`bazel --host_jvm_args=-Dhttps.proxyHost=127.0.0.1 --host_jvm_args=-Dhttps.proxyPort=10808 …`

---

## File Structure

| File | Responsibility | Phase |
|------|----------------|-------|
| `agentflow/dynamic/dynamic_state.h` / `.cc` | `MakeResumeTarget(reg, cp)` + `ParseStateBytes(...)` — build a dynamic `State` for `Runner::Resume`, parse untrusted bytes with limits | 1 |
| `agentflow/dynamic/schema_registry.{h,cc}` | (exists) extend `LoadDescriptorSet` with a size/recursion-bounded parse | 1 |
| `agentflow/dynamic/BUILD.bazel` | (exists) add `dynamic_state` sources + `//agentflow/core`, `//proto:agentflow_proto` deps | 1 |
| `tests/unit/dynamic/dynamic_state_test.cc` + `BUILD.bazel` | unit tests for the helpers | 1 |
| `tests/unit/dynamic/dynamic_resume_test.cc` | integration test: `Runner::Resume` with a `DynamicMessage` target on a real graph | 1 |
| `agentflow/jni/schema_registry_jni.cc` + `BUILD.bazel` | JNI shim: opaque handle over `SchemaRegistry`; create/destroy/load/has-type | 2 |
| `agentflow/jni/SchemaRegistryJni.kt` | Kotlin `external fun` declarations + `NativeLibraryLoader` | 2 |
| `kotlin/.../ProtoSchemaCompiler.kt` + `BUILD.bazel` | Wire `.proto` text → `FileDescriptorSet` bytes | 3 |
| `kotlin/.../ProtoSchemaCompilerTest.kt` | JVM JUnit test (protobuf-java cross-validation) | 3 |
| `kotlin/.../DynamicSchema.kt` | end-to-end Kotlin facade: compile → JNI load → has-type | 3 |
| `MODULE.bazel` | add `rules_kotlin` + Wire/protobuf-java maven deps | 3 |

---

## Phase 1 — C++ Integration & Hardening

*Runnable end-to-end in this repo today. Ship this phase first.*

### Task 1: `MakeResumeTarget` — build a dynamic State for Resume

`Runner::Resume(cp, target, …)` needs `target` to be an empty `State` of the checkpoint's concrete type. For dynamic schemas the caller has no compile-time type, so we build the target from the registry using `cp.state_type()`.

**Files:**
- Create: `agentflow/dynamic/dynamic_state.h`
- Create: `agentflow/dynamic/dynamic_state.cc`
- Modify: `agentflow/dynamic/BUILD.bazel`
- Test: `tests/unit/dynamic/dynamic_state_test.cc`
- Test BUILD: `tests/unit/dynamic/BUILD.bazel` (add target)

- [ ] **Step 1: Write the failing test**

Create `tests/unit/dynamic/dynamic_state_test.cc`:

```cpp
// tests/unit/dynamic/dynamic_state_test.cc
#include "agentflow/dynamic/dynamic_state.h"

#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <gtest/gtest.h>

#include "agentflow/dynamic/schema_registry.h"
#include "checkpoint.pb.h"

namespace agentflow {
namespace {

// syntax="proto3"; package dyntest; message Person { string name = 1; int32 age = 2; }
std::string PersonDescriptorSet() {
  google::protobuf::FileDescriptorSet set;
  auto* file = set.add_file();
  file->set_name("dyntest/person.proto");
  file->set_package("dyntest");
  file->set_syntax("proto3");
  auto* msg = file->add_message_type();
  msg->set_name("Person");
  auto* name = msg->add_field();
  name->set_name("name");
  name->set_number(1);
  name->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  name->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);
  auto* age = msg->add_field();
  age->set_name("age");
  age->set_number(2);
  age->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  age->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT32);
  return set.SerializeAsString();
}

// Build serialized Person{name=..,age=..} bytes via the registry itself.
std::string PersonBytes(SchemaRegistry& reg, const std::string& name, int age) {
  auto m = reg.NewMessage("dyntest.Person");
  EXPECT_TRUE(m.ok());
  const auto* d = (*m)->GetDescriptor();
  const auto* r = (*m)->GetReflection();
  r->SetString(m->get(), d->FindFieldByName("name"), name);
  r->SetInt32(m->get(), d->FindFieldByName("age"), age);
  return (*m)->SerializeAsString();
}

TEST(MakeResumeTargetTest, BuildsStateWhoseTypeMatchesCheckpoint) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());

  proto::Checkpoint cp;
  cp.set_state_type("dyntest.Person");
  cp.set_state_bytes(PersonBytes(reg, "alice", 30));

  auto target = MakeResumeTarget(reg, cp);
  ASSERT_TRUE(target.ok()) << target.status();
  ASSERT_NE(target->UnsafeMessage(), nullptr);
  EXPECT_EQ(target->UnsafeMessage()->GetTypeName(), "dyntest.Person");
}

TEST(MakeResumeTargetTest, UnknownTypeReturnsNotFound) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());

  proto::Checkpoint cp;
  cp.set_state_type("dyntest.Ghost");

  auto target = MakeResumeTarget(reg, cp);
  EXPECT_FALSE(target.ok());
  EXPECT_EQ(target.status().code(), absl::StatusCode::kNotFound);
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 2: Create the header**

Create `agentflow/dynamic/dynamic_state.h`:

```cpp
// agentflow/dynamic/dynamic_state.h
#ifndef AGENTFLOW_DYNAMIC_DYNAMIC_STATE_H_
#define AGENTFLOW_DYNAMIC_DYNAMIC_STATE_H_

#include "absl/status/statusor.h"

#include "agentflow/core/state.h"
#include "agentflow/dynamic/schema_registry.h"

namespace agentflow {

namespace proto { class Checkpoint; }  // proto/checkpoint.proto

// Builds an empty State of the checkpoint's declared type, ready to hand to
// Runner::Resume. The State holds a DynamicMessage backed by `reg`'s pool, so
// `reg` MUST outlive the returned State. Returns NotFound if cp.state_type()
// has not been loaded into `reg`.
absl::StatusOr<State> MakeResumeTarget(SchemaRegistry& reg,
                                       const proto::Checkpoint& cp);

}  // namespace agentflow

#endif  // AGENTFLOW_DYNAMIC_DYNAMIC_STATE_H_
```

- [ ] **Step 3: Add a stub implementation (so the test builds and fails)**

Create `agentflow/dynamic/dynamic_state.cc`:

```cpp
// agentflow/dynamic/dynamic_state.cc
#include "agentflow/dynamic/dynamic_state.h"

#include "checkpoint.pb.h"

namespace agentflow {

absl::StatusOr<State> MakeResumeTarget(SchemaRegistry& reg,
                                       const proto::Checkpoint& cp) {
  return absl::UnimplementedError("MakeResumeTarget");  // STUB
}

}  // namespace agentflow
```

- [ ] **Step 4: Wire up BUILD targets**

In `agentflow/dynamic/BUILD.bazel`, change the `dynamic` library to include the new sources and deps:

```python
cc_library(
    name = "dynamic",
    srcs = [
        "dynamic_state.cc",
        "schema_registry.cc",
    ],
    hdrs = [
        "dynamic_state.h",
        "schema_registry.h",
    ],
    deps = [
        "//agentflow/core",
        "//proto:agentflow_proto",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@com_google_protobuf//:protobuf",
    ],
)
```

In `tests/unit/dynamic/BUILD.bazel`, add:

```python
cc_test(
    name = "dynamic_state_test",
    size = "small",
    srcs = ["dynamic_state_test.cc"],
    deps = [
        "//agentflow/dynamic",
        "//proto:agentflow_proto",
        "@com_google_protobuf//:protobuf",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 5: Run test to verify it fails**

Run: `bazel --host_jvm_args=-Dhttps.proxyHost=127.0.0.1 --host_jvm_args=-Dhttps.proxyPort=10808 test //tests/unit/dynamic:dynamic_state_test --test_output=errors`
Expected: FAIL — `UNIMPLEMENTED: MakeResumeTarget` on `BuildsStateWhoseTypeMatchesCheckpoint`.

- [ ] **Step 6: Implement**

Replace the body of `agentflow/dynamic/dynamic_state.cc`:

```cpp
// agentflow/dynamic/dynamic_state.cc
#include "agentflow/dynamic/dynamic_state.h"

#include <utility>

#include "checkpoint.pb.h"

namespace agentflow {

absl::StatusOr<State> MakeResumeTarget(SchemaRegistry& reg,
                                       const proto::Checkpoint& cp) {
  auto msg = reg.NewMessage(cp.state_type());
  if (!msg.ok()) return msg.status();
  return State::FromMessage(std::move(*msg));
}

}  // namespace agentflow
```

- [ ] **Step 7: Run test to verify it passes**

Run: `bazel … test //tests/unit/dynamic:dynamic_state_test --test_output=errors`
Expected: PASS (2 tests).

- [ ] **Step 8: Commit**

```bash
git add agentflow/dynamic/dynamic_state.h agentflow/dynamic/dynamic_state.cc \
        agentflow/dynamic/BUILD.bazel tests/unit/dynamic/dynamic_state_test.cc \
        tests/unit/dynamic/BUILD.bazel
git commit -m "feat(dynamic): MakeResumeTarget — build a DynamicMessage State for Runner::Resume"
```

---

### Task 2: `Runner::Resume` end-to-end with a DynamicMessage target

Prove the existing type-erased Resume path works unchanged when the target is a `DynamicMessage`: state_type check passes, `state_bytes` parse into the dynamic message, a node mutates it via reflection.

**Files:**
- Test: `tests/unit/dynamic/dynamic_resume_test.cc`
- Test BUILD: `tests/unit/dynamic/BUILD.bazel` (add target)

- [ ] **Step 1: Write the failing test**

Create `tests/unit/dynamic/dynamic_resume_test.cc`:

```cpp
// tests/unit/dynamic/dynamic_resume_test.cc
#include "agentflow/core/runner.h"

#include <chrono>
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <gtest/gtest.h>

#include "agentflow/core/graph.h"
#include "agentflow/core/stub_node.h"
#include "agentflow/dynamic/dynamic_state.h"
#include "agentflow/dynamic/schema_registry.h"
#include "checkpoint.pb.h"

namespace agentflow {
namespace {

using namespace std::chrono_literals;

std::string PersonDescriptorSet() {
  google::protobuf::FileDescriptorSet set;
  auto* file = set.add_file();
  file->set_name("dyntest/person.proto");
  file->set_package("dyntest");
  file->set_syntax("proto3");
  auto* msg = file->add_message_type();
  msg->set_name("Person");
  auto* name = msg->add_field();
  name->set_name("name");
  name->set_number(1);
  name->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  name->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);
  return set.SerializeAsString();
}

State ResumeSync(Graph g, Runner::Options opts, const proto::Checkpoint& cp,
                 State target) {
  asio::io_context io;
  Runner runner(std::move(g), opts);
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await runner.Resume(cp, std::move(target));
      },
      asio::use_future);
  io.run();
  return fut.get();
}

TEST(DynamicResumeTest, ResumesColdStartWithDynamicTargetAndMutatesViaReflection) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());

  // Checkpoint with no completed nodes == cold start with target as initial.
  proto::Checkpoint cp;
  cp.set_state_type("dyntest.Person");
  {
    auto seed = reg.NewMessage("dyntest.Person");
    ASSERT_TRUE(seed.ok());
    const auto* d = (*seed)->GetDescriptor();
    (*seed)->GetReflection()->SetString(seed->get(),
                                        d->FindFieldByName("name"), "alice");
    cp.set_state_bytes((*seed)->SerializeAsString());
  }

  // Single node appends "+ran" to Person.name via reflection.
  StubNode::Body append_ran = [](State& s) {
    auto* m = const_cast<google::protobuf::Message*>(s.UnsafeMessage());
    const auto* f = m->GetDescriptor()->FindFieldByName("name");
    const auto* r = m->GetReflection();
    r->SetString(m, f, r->GetString(*m, f) + "+ran");
  };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("only", 0ms, nullptr, append_ran));

  auto target = MakeResumeTarget(reg, cp);
  ASSERT_TRUE(target.ok()) << target.status();

  State out = ResumeSync(b.Build(), Runner::Options{}, cp, std::move(*target));

  const auto* m = out.UnsafeMessage();
  ASSERT_NE(m, nullptr);
  const auto* f = m->GetDescriptor()->FindFieldByName("name");
  EXPECT_EQ(m->GetReflection()->GetString(*m, f), "alice+ran");
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 2: Add the BUILD target**

In `tests/unit/dynamic/BUILD.bazel`, add:

```python
cc_test(
    name = "dynamic_resume_test",
    size = "small",
    srcs = ["dynamic_resume_test.cc"],
    deps = [
        "//agentflow/core",
        "//agentflow/dynamic",
        "//proto:agentflow_proto",
        "@asio",
        "@com_google_protobuf//:protobuf",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `bazel … test //tests/unit/dynamic:dynamic_resume_test --test_output=errors`
Expected: FAIL — first run before any code doubt; if `MakeResumeTarget`/Resume already satisfy it, this test will PASS immediately. If it PASSES on first run, that is the expected proof the type-erased path already supports DynamicMessage — **keep the test** (it is a regression guard, not a TDD failure). Document the pass in the commit message.

> Rationale: Task 1 already added the only new production code this behavior needs. Task 2 is a characterization/integration test of existing Resume semantics against a dynamic target. A first-run pass here is acceptable and expected.

- [ ] **Step 4: If it fails, fix and re-run**

If the failure is a real defect (e.g. Resume mishandles a dynamic target), debug with superpowers:systematic-debugging, fix in `agentflow/core/runner.cc`, and re-run until PASS. Do not edit the test to make it pass.

- [ ] **Step 5: Commit**

```bash
git add tests/unit/dynamic/dynamic_resume_test.cc tests/unit/dynamic/BUILD.bazel
git commit -m "test(dynamic): Runner::Resume round-trips a DynamicMessage target end-to-end"
```

---

### Task 3: Harden parsing of untrusted descriptor sets and state bytes

Third-party `.proto` and serialized State are untrusted. Bound recursion and size to prevent stack overflow / OOM on malicious input. Add `ParseStateBytes` and apply a bounded parse in `LoadDescriptorSet`.

**Files:**
- Modify: `agentflow/dynamic/dynamic_state.h` (add `ParseStateBytes` + limit constants)
- Modify: `agentflow/dynamic/dynamic_state.cc`
- Modify: `agentflow/dynamic/schema_registry.cc` (bounded FileDescriptorSet parse)
- Test: `tests/unit/dynamic/dynamic_state_test.cc` (add cases)

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/dynamic/dynamic_state_test.cc` (inside the anonymous namespace, add includes `<google/protobuf/io/coded_stream.h>` is NOT needed in the test):

```cpp
TEST(ParseStateBytesTest, RejectsBytesExceedingSizeLimit) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());
  auto m = reg.NewMessage("dyntest.Person");
  ASSERT_TRUE(m.ok());

  // A valid Person with a 1 KiB name, parsed under a 64-byte cap → rejected.
  const std::string big = PersonBytes(reg, std::string(1024, 'x'), 1);
  absl::Status st = ParseStateBytes(**m, big, /*max_depth=*/100,
                                    /*max_bytes=*/64);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), absl::StatusCode::kInvalidArgument);
}

TEST(ParseStateBytesTest, AcceptsWellFormedBytesUnderLimits) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.LoadDescriptorSet(PersonDescriptorSet()).ok());
  auto m = reg.NewMessage("dyntest.Person");
  ASSERT_TRUE(m.ok());

  const std::string bytes = PersonBytes(reg, "bob", 7);
  ASSERT_TRUE(ParseStateBytes(**m, bytes, 100, 1 << 20).ok());
  const auto* d = (*m)->GetDescriptor();
  EXPECT_EQ((*m)->GetReflection()->GetString((**m),
                                             d->FindFieldByName("name")), "bob");
}
```

- [ ] **Step 2: Run to verify failure**

Run: `bazel … test //tests/unit/dynamic:dynamic_state_test --test_output=errors`
Expected: FAIL to build — `ParseStateBytes` is not declared.

- [ ] **Step 3: Declare the API**

Add to `agentflow/dynamic/dynamic_state.h` (above the closing namespace):

```cpp
#include <string_view>

#include <google/protobuf/message.h>

#include "absl/status/status.h"

// ... inside namespace agentflow ...

// Default bounds for parsing untrusted serialized State.
inline constexpr int kDefaultMaxParseDepth = 100;
inline constexpr int kDefaultMaxParseBytes = 64 * 1024 * 1024;  // 64 MiB

// Parses `bytes` into `msg` with bounded recursion depth and total size, and
// requires the entire input to be consumed. Use for untrusted State payloads
// (e.g. a checkpoint from disk or a third party). Returns InvalidArgument on a
// malformed message, a limit breach, or trailing garbage.
absl::Status ParseStateBytes(google::protobuf::Message& msg,
                             std::string_view bytes,
                             int max_depth = kDefaultMaxParseDepth,
                             int max_bytes = kDefaultMaxParseBytes);
```

- [ ] **Step 4: Implement**

Add to `agentflow/dynamic/dynamic_state.cc` (add includes at top):

```cpp
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

// ... inside namespace agentflow ...

absl::Status ParseStateBytes(google::protobuf::Message& msg,
                             std::string_view bytes, int max_depth,
                             int max_bytes) {
  google::protobuf::io::ArrayInputStream array_in(
      bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedInputStream coded_in(&array_in);
  coded_in.SetRecursionLimit(max_depth);
  coded_in.SetTotalBytesLimit(max_bytes);
  msg.Clear();
  if (!msg.ParseFromCodedStream(&coded_in) ||
      !coded_in.ConsumedEntireMessage()) {
    return absl::InvalidArgumentError(
        "ParseStateBytes: malformed message or limit exceeded");
  }
  return absl::OkStatus();
}
```

- [ ] **Step 5: Bound the descriptor-set parse in SchemaRegistry**

In `agentflow/dynamic/schema_registry.cc`, replace the `set.ParseFromArray(...)` call in `LoadDescriptorSet` with a recursion-bounded parse (descriptors nest via nested messages):

```cpp
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

// ... in LoadDescriptorSet, replace the ParseFromArray block: ...
  google::protobuf::FileDescriptorSet set;
  {
    google::protobuf::io::ArrayInputStream array_in(
        serialized_fds.data(), static_cast<int>(serialized_fds.size()));
    google::protobuf::io::CodedInputStream coded_in(&array_in);
    coded_in.SetRecursionLimit(100);
    if (!set.ParseFromCodedStream(&coded_in) ||
        !coded_in.ConsumedEntireMessage()) {
      return absl::InvalidArgumentError(
          "SchemaRegistry: not a valid FileDescriptorSet");
    }
  }
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `bazel … test //tests/unit/dynamic:dynamic_state_test //tests/unit/dynamic:schema_registry_test --test_output=errors`
Expected: PASS (all cases, including the new limit tests and the unchanged malformed-set test).

- [ ] **Step 7: Commit**

```bash
git add agentflow/dynamic/dynamic_state.h agentflow/dynamic/dynamic_state.cc \
        agentflow/dynamic/schema_registry.cc tests/unit/dynamic/dynamic_state_test.cc
git commit -m "feat(dynamic): bound recursion/size when parsing untrusted descriptors and state"
```

---

### Task 4: Phase-1 regression gate

- [ ] **Step 1: Run the whole dynamic + core suite**

Run:
```bash
bazel --host_jvm_args=-Dhttps.proxyHost=127.0.0.1 --host_jvm_args=-Dhttps.proxyPort=10808 \
  test //tests/unit/dynamic/... //tests/unit/core/... --test_output=errors
```
Expected: all targets PASS. This is the green baseline before touching JNI.

- [ ] **Step 2: Commit (only if anything changed)** — otherwise skip.

---

## Phase 2 — JNI Bridge

*Thin marshalling only. All logic lives in the Phase-1 (tested) C++ layer. The JNI surface is verified by the Kotlin smoke test in Phase 3.*

### Task 5: JNI shim over SchemaRegistry

Expose four native functions: create a registry (returns an opaque `jlong` handle), load a descriptor set, test whether a type is present, and destroy. Handle is `reinterpret_cast<jlong>(SchemaRegistry*)`, mirroring `LiteRT-LM/kotlin/java/com/google/ai/edge/litertlm/jni/litertlm.cc` (`reinterpret_cast<jlong>(engine->release())`, `env->ThrowNew`, `GetByteArrayElements`).

**Files:**
- Create: `agentflow/jni/schema_registry_jni.cc`
- Create: `agentflow/jni/BUILD.bazel`

- [ ] **Step 1: Write the JNI implementation**

Create `agentflow/jni/schema_registry_jni.cc`:

```cpp
// agentflow/jni/schema_registry_jni.cc
#include <jni.h>

#include <memory>
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
  env->ThrowNew(cls, message.c_str());
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL JNI_METHOD(nativeCreate)(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(new agentflow::SchemaRegistry());
}

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
```

- [ ] **Step 2: Write the BUILD target**

Create `agentflow/jni/BUILD.bazel`:

```python
# agentflow/jni/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

# Shared library loaded by the JVM via System.loadLibrary("agentflow_jni").
cc_binary(
    name = "libagentflow_jni.so",
    srcs = ["schema_registry_jni.cc"],
    linkshared = True,
    linkstatic = True,
    deps = [
        "//agentflow/dynamic",
        "@bazel_tools//tools/jdk:jni",
    ],
)
```

- [ ] **Step 3: Verify it compiles and links**

Run: `bazel … build //agentflow/jni:libagentflow_jni.so`
Expected: `Build completed successfully`; artifact at `bazel-bin/agentflow/jni/libagentflow_jni.so`.

> Note: there is no C++ unit test here by design — JNI entry points need a live JVM. Correctness of the marshalling is covered by the Kotlin smoke test in Task 8. The *logic* (`LoadDescriptorSet`/`NewMessage`) is already unit-tested in Phase 1.

- [ ] **Step 4: Commit**

```bash
git add agentflow/jni/schema_registry_jni.cc agentflow/jni/BUILD.bazel
git commit -m "feat(jni): thin SchemaRegistry JNI shim (create/load/has-type/destroy)"
```

---

## Phase 3 — JVM/Kotlin Compiler & Facade

*The Kotlin compiler (Task 7) is pure-JVM and fully TDD-able. The facade smoke test (Task 8) needs the `.so` from Phase 2 on `java.library.path`.*

### Task 6: Add Kotlin + Wire to the build

**Files:**
- Modify: `MODULE.bazel`

- [ ] **Step 1: Add rules_kotlin and maven deps**

Append to `MODULE.bazel` (versions current as of 2026-06; pin exactly):

```python
bazel_dep(name = "rules_kotlin", version = "1.9.6")

bazel_dep(name = "rules_jvm_external", version = "6.2")
maven = use_extension("@rules_jvm_external//:extensions.bzl", "maven")
maven.install(
    name = "agentflow_maven",
    artifacts = [
        "com.squareup.wire:wire-schema:4.9.9",        # SchemaLoader + internal.SchemaEncoder
        "com.squareup.okio:okio:3.9.0",               # in-memory FileSystem
        "com.google.protobuf:protobuf-java:4.31.1",   # TEST-ONLY descriptor validation
        "junit:junit:4.13.2",                          # TEST-ONLY
    ],
    # Wire's SchemaEncoder lives in com.squareup.wire.schema.internal — pin the
    # version; treat any Wire upgrade as an API-break risk and re-run Task 7.
)
use_repo(maven, "agentflow_maven")
```

- [ ] **Step 2: Verify the deps resolve**

Run: `bazel … build @agentflow_maven//:com_squareup_wire_wire_schema`
Expected: fetches and builds without error. (Needs the proxy JVM args — Wire pulls from Maven Central.)

- [ ] **Step 3: Commit**

```bash
git add MODULE.bazel MODULE.bazel.lock
git commit -m "build: add rules_kotlin + Wire/okio/protobuf-java(maven) for dynamic schema"
```

---

### Task 7: `ProtoSchemaCompiler` — `.proto` text → FileDescriptorSet

Parse a set of `.proto` source files (path → content) entirely in memory with Wire's `SchemaLoader`, encode each file with `SchemaEncoder`, and assemble a `FileDescriptorSet`. No filesystem, no `protoc`.

**Files:**
- Create: `kotlin/java/com/google/ai/edge/agentflow/schema/ProtoSchemaCompiler.kt`
- Create: `kotlin/java/com/google/ai/edge/agentflow/schema/BUILD.bazel`
- Test: `kotlin/java/com/google/ai/edge/agentflow/schema/ProtoSchemaCompilerTest.kt`

- [ ] **Step 1: Write the failing test**

Create `ProtoSchemaCompilerTest.kt`:

```kotlin
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

    // protobuf-java must accept the set Wire produced — this is the JNI contract.
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

    // Build with dependency resolution — proves topological order is correct.
    val byName = set.fileList.associateBy { it.name }
    val addrFd = Descriptors.FileDescriptor.buildFrom(
      byName["dyntest/addr.proto"]!!, emptyArray())
    val personFd = Descriptors.FileDescriptor.buildFrom(
      byName["dyntest/person.proto"]!!, arrayOf(addrFd))
    assertTrue(personFd.findMessageTypeByName("Person")
      .findFieldByName("addr").messageType.fullName == "dyntest.Addr")
  }
}
```

- [ ] **Step 2: Create the BUILD target**

Create `kotlin/java/com/google/ai/edge/agentflow/schema/BUILD.bazel`:

```python
# kotlin/java/com/google/ai/edge/agentflow/schema/BUILD.bazel
load("@rules_kotlin//kotlin:jvm.bzl", "kt_jvm_library", "kt_jvm_test")

package(default_visibility = ["//visibility:public"])

kt_jvm_library(
    name = "schema",
    srcs = ["ProtoSchemaCompiler.kt"],
    deps = [
        "@agentflow_maven//:com_squareup_wire_wire_schema",
        "@agentflow_maven//:com_squareup_okio_okio",
    ],
)

kt_jvm_test(
    name = "ProtoSchemaCompilerTest",
    srcs = ["ProtoSchemaCompilerTest.kt"],
    test_class = "com.google.ai.edge.agentflow.schema.ProtoSchemaCompilerTest",
    deps = [
        ":schema",
        "@agentflow_maven//:com_google_protobuf_protobuf_java",
        "@agentflow_maven//:junit_junit",
    ],
)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `bazel … test //kotlin/java/com/google/ai/edge/agentflow/schema:ProtoSchemaCompilerTest --test_output=errors`
Expected: FAIL to compile — `ProtoSchemaCompiler` does not exist.

- [ ] **Step 4: Implement**

Create `ProtoSchemaCompiler.kt`:

```kotlin
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
 * `google.protobuf.FileDescriptorSet`, suitable for a C++ DescriptorPool.
 *
 * Uses Wire's SchemaLoader (parse) + SchemaEncoder (encode). SchemaEncoder is
 * in Wire's `internal` package: its output is protoc-compatible (verified by
 * Wire's own SchemaEncoderInteropTest) but the API has no stability guarantee.
 * Pin the Wire version; re-run the tests on any upgrade.
 */
class ProtoSchemaCompiler {

  /** @param protoFiles map of proto import path -> source text. */
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
    // Emit only the source files (not Wire's bundled well-known types), in the
    // input order; SchemaLoader has already validated imports resolve. Encode
    // each as a length-delimited field #1.
    val out = Buffer()
    for (path in protoFiles.keys) {
      val protoFile = schema.protoFile(path)
        ?: error("schema did not load expected file: $path")
      val fdpBytes: ByteString = encoder.encode(protoFile)
      // tag = (field 1 << 3) | wiretype 2 (LEN) = 0x0A
      out.writeByte(0x0A)
      out.writeUtf8CodePoint(0)  // placeholder; replaced below via varint
      // (Simpler: use a helper that writes a proper varint length.)
      error("replaced in Step 5")  // see note
    }
    return out.readByteArray()
  }
}
```

> The naive length-prefix above is intentionally wrong so Step 5 fixes it with a correct varint writer. (Keeps the TDD red/green honest: Step 4 builds, Step 3's compile error is resolved, but the test still fails on bad output.)

- [ ] **Step 5: Run test, watch it fail on output, then fix the assembly**

Run the test (expect FAIL via the `error("replaced in Step 5")`), then replace the `compile` body's assembly loop with a correct `FileDescriptorSet` writer:

```kotlin
    val out = Buffer()
    for (path in protoFiles.keys) {
      val protoFile = schema.protoFile(path)
        ?: error("schema did not load expected file: $path")
      val fdpBytes: ByteString = encoder.encode(protoFile)
      out.writeByte(0x0A)              // field 1, wiretype LEN
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
```

- [ ] **Step 6: Run test to verify it passes**

Run: `bazel … test //kotlin/java/com/google/ai/edge/agentflow/schema:ProtoSchemaCompilerTest --test_output=errors`
Expected: PASS (both tests). protobuf-java parsing and `buildFrom` succeeding is the proof the bytes are C++-pool-loadable.

- [ ] **Step 7: Commit**

```bash
git add kotlin/java/com/google/ai/edge/agentflow/schema/
git commit -m "feat(schema): ProtoSchemaCompiler — Wire .proto text -> FileDescriptorSet bytes"
```

---

### Task 8: Kotlin facade + end-to-end smoke test

Wire the compiler to the JNI shim: declare the `external fun`s, load the `.so`, and prove a `.proto` string compiled in Kotlin is accepted by the C++ `SchemaRegistry` and resolves its type.

**Files:**
- Create: `agentflow/jni/SchemaRegistryJni.kt`
- Create: `kotlin/java/com/google/ai/edge/agentflow/schema/DynamicSchema.kt`
- Create: `kotlin/java/com/google/ai/edge/agentflow/schema/DynamicSchemaSmokeTest.kt`
- Modify: `agentflow/jni/BUILD.bazel` (add `kt_jvm_library` for the bindings), `kotlin/.../schema/BUILD.bazel` (add smoke test, data-dep the `.so`)

- [ ] **Step 1: Write the JNI Kotlin bindings**

Create `agentflow/jni/SchemaRegistryJni.kt`:

```kotlin
package com.google.ai.edge.agentflow.jni

/** 1:1 binding to agentflow/jni/schema_registry_jni.cc. */
object SchemaRegistryJni {
  init { System.loadLibrary("agentflow_jni") }

  external fun nativeCreate(): Long
  external fun nativeDestroy(handle: Long)
  /** Throws IllegalStateException with the absl::Status message on failure. */
  external fun nativeLoadDescriptorSet(handle: Long, fds: ByteArray)
  external fun nativeHasType(handle: Long, fullTypeName: String): Boolean
}
```

- [ ] **Step 2: Write the facade**

Create `DynamicSchema.kt`:

```kotlin
package com.google.ai.edge.agentflow.schema

import com.google.ai.edge.agentflow.jni.SchemaRegistryJni

/** Owns a native SchemaRegistry. Close it to free native memory. */
class DynamicSchema : AutoCloseable {
  private val handle = SchemaRegistryJni.nativeCreate()
  private val compiler = ProtoSchemaCompiler()

  /** Compile `.proto` text and load it into the native registry. */
  fun load(protoFiles: Map<String, String>) {
    SchemaRegistryJni.nativeLoadDescriptorSet(handle, compiler.compile(protoFiles))
  }

  fun hasType(fullTypeName: String): Boolean =
    SchemaRegistryJni.nativeHasType(handle, fullTypeName)

  override fun close() = SchemaRegistryJni.nativeDestroy(handle)
}
```

- [ ] **Step 3: Write the failing smoke test**

Create `DynamicSchemaSmokeTest.kt`:

```kotlin
package com.google.ai.edge.agentflow.schema

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DynamicSchemaSmokeTest {
  @Test fun `proto compiled in kotlin is loadable and resolvable in native registry`() {
    DynamicSchema().use { schema ->
      schema.load(mapOf("dyntest/person.proto" to """
        syntax = "proto3";
        package dyntest;
        message Person { string name = 1; int32 age = 2; }
      """.trimIndent()))
      assertTrue(schema.hasType("dyntest.Person"))
      assertFalse(schema.hasType("dyntest.Ghost"))
    }
  }
}
```

- [ ] **Step 4: Wire BUILD targets**

Add to `agentflow/jni/BUILD.bazel`:

```python
load("@rules_kotlin//kotlin:jvm.bzl", "kt_jvm_library")

kt_jvm_library(
    name = "schema_registry_jni_kt",
    srcs = ["SchemaRegistryJni.kt"],
)
```

Add to `kotlin/java/com/google/ai/edge/agentflow/schema/BUILD.bazel`:

```python
kt_jvm_library(
    name = "dynamic_schema",
    srcs = ["DynamicSchema.kt"],
    deps = [
        ":schema",
        "//agentflow/jni:schema_registry_jni_kt",
    ],
)

kt_jvm_test(
    name = "DynamicSchemaSmokeTest",
    srcs = ["DynamicSchemaSmokeTest.kt"],
    test_class = "com.google.ai.edge.agentflow.schema.DynamicSchemaSmokeTest",
    data = ["//agentflow/jni:libagentflow_jni.so"],
    jvm_flags = [
        "-Djava.library.path=$(BINDIR)/agentflow/jni",
    ],
    deps = [
        ":dynamic_schema",
        "@agentflow_maven//:junit_junit",
    ],
)
```

- [ ] **Step 5: Run smoke test to verify it fails, then passes**

Run: `bazel … test //kotlin/java/com/google/ai/edge/agentflow/schema:DynamicSchemaSmokeTest --test_output=errors`
Expected first run: FAIL (classes/targets not yet built or `UnsatisfiedLinkError` if `java.library.path` is wrong — adjust the `jvm_flags` path to where Bazel stages the `.so`; confirm with `bazel cquery --output=files //agentflow/jni:libagentflow_jni.so`).
Expected after fixing the library path: PASS — `hasType("dyntest.Person")` true, `"dyntest.Ghost"` false. This proves the full chain: Kotlin `.proto` text → Wire → FileDescriptorSet → JNI → C++ `DescriptorPool`.

- [ ] **Step 6: Commit**

```bash
git add agentflow/jni/SchemaRegistryJni.kt agentflow/jni/BUILD.bazel \
        kotlin/java/com/google/ai/edge/agentflow/schema/DynamicSchema.kt \
        kotlin/java/com/google/ai/edge/agentflow/schema/DynamicSchemaSmokeTest.kt \
        kotlin/java/com/google/ai/edge/agentflow/schema/BUILD.bazel
git commit -m "feat(schema): DynamicSchema facade + end-to-end JNI smoke test"
```

---

## Out of Scope (future plans)

- **Android packaging**: per-ABI `.so` splits, `arm64-v8a` NDK toolchain config, `AndroidManifest`/AAR. This plan targets the host JVM via Bazel `kt_jvm_test`; the native + Kotlin code is portable but the Android build harness is a separate effort.
- **Driving graph nodes by dynamic field names**: `agent_node.cc` etc. already use reflection; a follow-up should confirm node configs reference fields that exist in the loaded dynamic schema (validation at graph-compile time).
- **Wire imports of google well-known types** (`timestamp.proto`, etc.): if third-party `.proto` imports them, seed both Wire's `SchemaLoader` source path and the C++ pool. The current `proto/` set uses none.

---

## Self-Review

- **Spec coverage** (the four B-items from the brainstorm): JVM Wire parse+encode → Task 7; assemble FileDescriptorSet → Task 7 Step 5; JNI boundary → Tasks 5 & 8; runner/checkpoint Resume integration → Tasks 1 & 2; robustness/limits → Task 3. ✓
- **Type consistency**: `SchemaRegistry::{LoadDescriptorSet,NewMessage}`, `State::FromMessage`, `MakeResumeTarget(reg, cp)`, `ParseStateBytes(msg, bytes, depth, bytes)`, `nativeCreate/nativeDestroy/nativeLoadDescriptorSet/nativeHasType`, `ProtoSchemaCompiler.compile(Map)`, `DynamicSchema.{load,hasType,close}` — names match across C++/Kotlin/JNI tasks. ✓
- **Placeholder scan**: every code step has full code; the one deliberately-wrong stub (Task 7 Step 4) is called out and fixed in Step 5 to preserve a real red→green. ✓
- **Known risk surfaced**: `SchemaEncoder` is in Wire's `internal` package (pin version; re-test on upgrade) — noted in Tasks 6 & 7.
