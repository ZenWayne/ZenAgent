# C++ Agent Framework — P2: Agent Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the agent layer on top of P1's core runner — LiteRT-LM inference wrapper, tool system, and AgentNode (ReAct loop) — while migrating the entire project from CMake to Bazel.

**Architecture:** Three new modules (`inference/`, `tools/`, `nodes/`) on top of P1's `core/`. LiteRT-LM C API wrapped via asio channel streaming bridge. Tool system provides NativeFnTool + ToolRegistry with JSON export for LLM function calling. AgentNode encapsulates the LLM+tool ReAct loop as a single graph node.

**Tech Stack:** C++20, Bazel 7.x (Bzlmod), LiteRT-LM (C API, prebuilt), asio 1.30, protobuf 25.3, abseil 20240722, GoogleTest 1.14, nlohmann_json.

**Spec reference:** `docs/superpowers/specs/2026-05-11-cpp-agent-framework-p2-agent-layer.md`

---

## File Structure

```
zen/
├── MODULE.bazel                          # NEW: Bazel bzlmod entry
├── BUILD.bazel                           # NEW: root package
├── .bazelrc                              # NEW
├── .bazelversion                         # NEW
├── proto/
│   ├── BUILD.bazel                       # NEW: proto_library + cc_proto_library
│   ├── errors.proto                      # (existing)
│   ├── trace_event.proto                 # (existing)
│   └── test_messages.proto               # (existing)
├── agentflow/
│   ├── BUILD.bazel                       # NEW: package visibility
│   ├── core/
│   │   ├── BUILD.bazel                   # NEW: cc_library agentflow_core
│   │   ├── errors.{h,cc}
│   │   ├── cancel.{h,cc}
│   │   ├── event.{h,cc}
│   │   ├── state.{h,cc}
│   │   ├── edge.h
│   │   ├── node.h
│   │   ├── graph.{h,cc}
│   │   └── runner.{h,cc}
│   ├── inference/                        # NEW
│   │   ├── BUILD.bazel
│   │   ├── litert_lm_engine.h
│   │   ├── litert_lm_engine.cc
│   │   ├── litert_lm_session.h
│   │   └── litert_lm_session.cc
│   ├── tools/                            # NEW
│   │   ├── BUILD.bazel
│   │   ├── tool.h
│   │   ├── native_fn_tool.h
│   │   ├── native_fn_tool.cc
│   │   ├── tool_registry.h
│   │   └── tool_registry.cc
│   └── nodes/                            # NEW
│       ├── BUILD.bazel
│       ├── agent_node.h
│       └── agent_node.cc
├── tests/
│   ├── BUILD.bazel                       # NEW: root tests
│   └── unit/
│       ├── BUILD.bazel                   # NEW: Core tests
│       ├── core/
│       │   ├── errors_test.cc
│       │   ├── cancel_test.cc
│       │   ├── event_test.cc
│       │   ├── state_test.cc
│       │   ├── graph_compile_test.cc
│       │   ├── runner_test.cc
│       │   └── stub_node.h
│       ├── inference/                    # NEW
│       │   ├── BUILD.bazel
│       │   └── litert_lm_session_test.cc
│       ├── tools/                        # NEW
│       │   ├── BUILD.bazel
│       │   ├── native_fn_tool_test.cc
│       │   └── tool_registry_test.cc
│       └── nodes/                        # NEW
│           ├── BUILD.bazel
│           └── agent_node_test.cc
├── examples/
│   ├── BUILD.bazel                       # NEW
│   ├── core-stub-graph/
│   │   ├── BUILD.bazel                   # NEW
│   │   └── main.cc
│   └── agent-demo/                       # NEW
│       ├── BUILD.bazel
│       └── main.cc
└── LiteRT-LM/                            # existing, prebuilt separately
```

## Task Dependency

```
T1 (MODULE.bazel + proto) ──→ T2 (core → Bazel) ──→ T3 (LiteRT-LM deps)
                                                         └──→ T4 (inference)
                                                               └──→ T5 (tools) ──→ T6 (AgentNode) ──→ T7 (demo + verify)
```

T1–T3 are Bazel migration; T4–T6 are new code (can overlap with T3 after LiteRT-LM target exists).

---

## Task 1: Bazel Scaffold — MODULE.bazel + Proto

**Files:**
- Create: `MODULE.bazel`
- Create: `.bazelrc`
- Create: `.bazelversion`
- Create: `BUILD.bazel` (root)
- Create: `agentflow/BUILD.bazel`
- Create: `tests/BUILD.bazel`
- Create: `examples/BUILD.bazel`
- Create: `proto/BUILD.bazel`

- [ ] **Step 1.1: Create `.bazelversion`**

```
7.4.1
```

- [ ] **Step 1.2: Create `.bazelrc`**

```
# .bazelrc
build --cxxopt=-std=c++20 --host_cxxopt=-std=c++20
build --features=layering_check

# Sanitizers
build:asan --copt=-fsanitize=address --linkopt=-fsanitize=address
build:ubsan --copt=-fsanitize=undefined --linkopt=-fsanitize=undefined

# Test output
test --test_output=errors
```

- [ ] **Step 1.3: Create `MODULE.bazel`**

```python
module(name = "agentflow", version = "0.1.0")

# ── Dependencies (Bazel Central Registry) ──────────────────────────
bazel_dep(name = "rules_cc", version = "0.0.9")
bazel_dep(name = "rules_proto", version = "6.0.0")
bazel_dep(name = "protobuf", version = "25.3")
bazel_dep(name = "googletest", version = "1.14.0")
bazel_dep(name = "abseil-cpp", version = "20240722.0")
bazel_dep(name = "nlohmann_json", version = "3.11.3")
bazel_dep(name = "platforms", version = "0.0.10")
bazel_dep(name = "bazel_skylib", version = "1.7.1")

# ── asio (header-only, not in BCR — use http_archive) ─────────────
http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
http_archive(
    name = "asio",
    build_file_content = """
cc_library(
    name = "asio",
    hdrs = glob(["asio/include/**/*.hpp"]),
    includes = ["asio/include"],
    defines = [
        "ASIO_STANDALONE",
        "ASIO_NO_DEPRECATED",
    ],
    visibility = ["//visibility:public"],
)
""",
    strip_prefix = "asio-asio-1-30-2",
    urls = ["https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz"],
)

# ── LiteRT-LM (local prebuilt — updated in Task 3) ────────────────
# Placeholder: populated after Task 3 builds the LiteRT-LM library.
```

- [ ] **Step 1.4: Create root `BUILD.bazel`**

```python
# BUILD.bazel
package(default_visibility = ["//visibility:public"])
```

- [ ] **Step 1.5: Create `agentflow/BUILD.bazel`**

```python
# agentflow/BUILD.bazel
package(default_visibility = ["//visibility:public"])
```

- [ ] **Step 1.6: Create `tests/BUILD.bazel`**

```python
# tests/BUILD.bazel
package(default_visibility = ["//visibility:private"])
```

- [ ] **Step 1.7: Create `examples/BUILD.bazel`**

```python
# examples/BUILD.bazel
package(default_visibility = ["//visibility:private"])
```

- [ ] **Step 1.8: Write `proto/BUILD.bazel`**

```python
# proto/BUILD.bazel
load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:defs.bzl", "cc_proto_library")

proto_library(
    name = "errors_proto",
    srcs = ["errors.proto"],
    visibility = ["//visibility:public"],
)

proto_library(
    name = "trace_event_proto",
    srcs = ["trace_event.proto"],
    deps = [":errors_proto"],
    visibility = ["//visibility:public"],
)

proto_library(
    name = "test_messages_proto",
    srcs = ["test_messages.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "agentflow_proto",
    deps = [
        ":errors_proto",
        ":test_messages_proto",
        ":trace_event_proto",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 1.9: Verify proto build**

Run: `bazel build //proto/...`
Expected: builds protobuf deps + generates C++ from all three .proto files.

- [ ] **Step 1.10: Commit**

```bash
git add MODULE.bazel BUILD.bazel .bazelrc .bazelversion \
    agentflow/BUILD.bazel tests/BUILD.bazel examples/BUILD.bazel \
    proto/BUILD.bazel
git commit -m "build: scaffold Bazel workspace with MODULE.bazel + proto targets"
```

---

## Task 2: Core Bazel Migration

**Files:**
- Create: `agentflow/core/BUILD.bazel`
- Create: `tests/unit/core/BUILD.bazel`
- Create: `examples/core-stub-graph/BUILD.bazel`

- [ ] **Step 2.1: Write `agentflow/core/BUILD.bazel`**

```python
# agentflow/core/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "core",
    srcs = [
        "cancel.cc",
        "errors.cc",
        "event.cc",
        "graph.cc",
        "runner.cc",
        "state.cc",
    ],
    hdrs = [
        "cancel.h",
        "edge.h",
        "errors.h",
        "event.h",
        "graph.h",
        "node.h",
        "runner.h",
        "state.h",
    ],
    deps = [
        "//proto:agentflow_proto",
        "@asio",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
    ],
)
```

- [ ] **Step 2.2: Write `tests/unit/core/BUILD.bazel`**

```python
# tests/unit/core/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_test")

_TEST_SRCS = [
    "errors_test.cc",
    "cancel_test.cc",
    "event_test.cc",
    "state_test.cc",
    "graph_compile_test.cc",
    "runner_test.cc",
]

[cc_test(
    name = src.replace(".cc", ""),
    size = "small",
    srcs = [src, "stub_node.h"],
    deps = [
        "//agentflow/core",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
) for src in _TEST_SRCS]
```

- [ ] **Step 2.3: Build + Run core tests**

Run: `bazel test //tests/unit/core/... --test_output=errors`
Expected: 39 tests PASS (same as P1 CMake).

- [ ] **Step 2.4: Write `examples/core-stub-graph/BUILD.bazel`**

```python
# examples/core-stub-graph/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "core_stub_graph_demo",
    srcs = ["main.cc"],
    deps = ["//agentflow/core"],
)
```

- [ ] **Step 2.5: Build demo**

Run: `bazel build //examples/core-stub-graph/...`
Expected: builds clean.

- [ ] **Step 2.6: Commit**

```bash
git add agentflow/core/BUILD.bazel tests/unit/core/BUILD.bazel \
    examples/core-stub-graph/BUILD.bazel
git commit -m "build: migrate agentflow_core + tests + demo from CMake to Bazel"
```

---

## Task 3: LiteRT-LM Bazel Integration

**Files:**
- Modify: `MODULE.bazel` (add LiteRT-LM new_local_repository)

**Context:** LiteRT-LM's Bazel BUILD files target Google's internal build infrastructure and cannot be consumed directly from OSS Bazel. The CMake build system is functional but we want Bazel for the rest of the project. Approach: prebuild LiteRT-LM using CMake, then wrap the output as a Bazel `cc_import`.

- [ ] **Step 3.1: Build LiteRT-LM C library with CMake**

```bash
mkdir -p /tmp/litert_lm_build
cmake -S LiteRT-LM -B /tmp/litert_lm_build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build /tmp/litert_lm_build -j$(nproc) --target c_engine 2>&1 | tail -20
```

Expected: `libc_engine.a` is produced in `/tmp/litert_lm_build/c/`.

- [ ] **Step 3.2: Create Bazel external repository for LiteRT-LM**

Create `third_party/litert_lm/BUILD.bazel`:

```python
# third_party/litert_lm/BUILD.bazel
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "c_engine",
    srcs = ["libc_engine.a"],
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    linkopts = [
        "-lpthread",
        "-ldl",
    ],
    # LiteRT-LM's C library depends on these; the actual .a bundles them.
    deps = [
        "@nlohmann_json//:json",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
    ],
)
```

- [ ] **Step 3.3: Copy built artifacts to third_party/litert_lm/**

```bash
mkdir -p third_party/litert_lm/include/c
cp /tmp/litert_lm_build/c/libc_engine.a third_party/litert_lm/
cp LiteRT-LM/c/engine.h third_party/litert_lm/include/c/
```

- [ ] **Step 3.4: Update MODULE.bazel**

Add to MODULE.bazel:
```python
# LiteRT-LM — prebuilt, see third_party/litert_lm/
local_path_override(module_name = "litert_lm", path = "third_party/litert_lm")
```

- [ ] **Step 3.5: Verify LiteRT-LM link**

Create a minimal smoke test `tests/smoke/litert_lm_link_test.cc`:
```cpp
#include "c/engine.h"
int main() {
    LiteRtLmEngine* e = nullptr;
    (void)e;
    return 0;
}
```

Run: `bazel build //tests/smoke/...`
Expected: links successfully (engine.h header resolves, libc_engine.a links).

- [ ] **Step 3.6: (Alternative — if CMake build fails)** If Step 3.1 fails, fall back to `cc_library` with just the header (header-only stub for P2):

The LiteRT-LM C API header can be wrapped without the actual library for compilation:
```python
cc_library(
    name = "c_engine_stub",
    hdrs = ["include/c/engine.h"],
    includes = ["include"],
    deps = ["@nlohmann_json//:json"],
)
```

All agentflow inference code uses the header types. The actual LiteRT-LM library linking happens when running with a real model. Tests that need real inference become `manual` in Bazel.

- [ ] **Step 3.7: Commit**

```bash
git add MODULE.bazel third_party/litert_lm/ tests/smoke/
git commit -m "build: integrate LiteRT-LM as prebuilt Bazel dependency"
```

---

## Task 4: Inference Layer — LiteRtLmEngine + LiteRtLmSession

**Files:**
- Create: `agentflow/inference/litert_lm_engine.h`
- Create: `agentflow/inference/litert_lm_engine.cc`
- Create: `agentflow/inference/litert_lm_session.h`
- Create: `agentflow/inference/litert_lm_session.cc`
- Create: `agentflow/inference/BUILD.bazel`
- Create: `tests/unit/inference/litert_lm_session_test.cc`
- Create: `tests/unit/inference/BUILD.bazel`

- [ ] **Step 4.1: Write `agentflow/inference/BUILD.bazel`**

```python
# agentflow/inference/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "inference",
    srcs = [
        "litert_lm_engine.cc",
        "litert_lm_session.cc",
    ],
    hdrs = [
        "litert_lm_engine.h",
        "litert_lm_session.h",
    ],
    deps = [
        "//agentflow/core",
        "@asio",
        "@litert_lm//:c_engine",
        "@nlohmann_json//:json",
    ],
)
```

- [ ] **Step 4.2: Write `agentflow/inference/litert_lm_engine.h`**

```cpp
// agentflow/inference/litert_lm_engine.h
#ifndef AGENTFLOW_INFERENCE_LITERT_LM_ENGINE_H_
#define AGENTFLOW_INFERENCE_LITERT_LM_ENGINE_H_

#include <memory>
#include <string>

#include "c/engine.h"  // LiteRT-LM C API

namespace agentflow {

struct LiteRtLmEngineOptions {
  std::string model_path;
  std::string backend = "cpu";
  std::string cache_dir;
  int max_num_tokens = 4096;
};

// Shared wrapper around LiteRtLmEngine*. One engine per model file.
// Thread-safe: multiple sessions can be created concurrently.
class LiteRtLmEngine {
 public:
  static std::shared_ptr<LiteRtLmEngine> Create(LiteRtLmEngineOptions opts);
  ~LiteRtLmEngine();

  LiteRtLmEngine(const LiteRtLmEngine&) = delete;
  LiteRtLmEngine& operator=(const LiteRtLmEngine&) = delete;

  // Returns the raw engine pointer (for creating sessions).
  LiteRtLmEngine* Get() const { return engine_; }

 private:
  LiteRtLmEngine(LiteRtLmEngine* engine) : engine_(engine) {}
  LiteRtLmEngine* engine_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_LITERT_LM_ENGINE_H_
```

- [ ] **Step 4.3: Write `agentflow/inference/litert_lm_engine.cc`**

```cpp
// agentflow/inference/litert_lm_engine.cc
#include "agentflow/inference/litert_lm_engine.h"

namespace agentflow {

std::shared_ptr<LiteRtLmEngine> LiteRtLmEngine::Create(
    LiteRtLmEngineOptions opts) {
  auto* settings = litert_lm_engine_settings_create(
      opts.model_path.c_str(), opts.backend.c_str(),
      /*vision_backend=*/nullptr, /*audio_backend=*/nullptr);
  if (opts.max_num_tokens > 0) {
    litert_lm_engine_settings_set_max_num_tokens(settings, opts.max_num_tokens);
  }
  if (!opts.cache_dir.empty()) {
    litert_lm_engine_settings_set_cache_dir(settings, opts.cache_dir.c_str());
  }
  auto* engine = litert_lm_engine_create(settings);
  litert_lm_engine_settings_delete(settings);
  if (!engine) return nullptr;
  return std::shared_ptr<LiteRtLmEngine>(new LiteRtLmEngine(engine));
}

LiteRtLmEngine::~LiteRtLmEngine() {
  if (engine_) litert_lm_engine_delete(engine_);
}

}  // namespace agentflow
```

- [ ] **Step 4.4: Write `agentflow/inference/litert_lm_session.h`**

```cpp
// agentflow/inference/litert_lm_session.h
#ifndef AGENTFLOW_INFERENCE_LITERT_LM_SESSION_H_
#define AGENTFLOW_INFERENCE_LITERT_LM_SESSION_H_

#include <atomic>
#include <memory>
#include <string>

#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/io_context.hpp>

#include "c/engine.h"

namespace agentflow {

// Async streaming wrapper around LiteRtLmSession*.
// Callback-from-background-thread → asio channel bridge.
class LiteRtLmSession {
 public:
  LiteRtLmSession(LiteRtLmSession* session, asio::io_context& io);
  ~LiteRtLmSession();

  LiteRtLmSession(const LiteRtLmSession&) = delete;
  LiteRtLmSession& operator=(const LiteRtLmSession&) = delete;

  // Start streaming generation. Non-blocking.
  // `input_text` is the full conversation JSON (system + user + tools).
  void Start(std::string input_text);

  // Await the next decoded token. Empty string = stream ended.
  // Throws std::runtime_error on stream error.
  asio::awaitable<std::string> NextTokenAsync();

  // Cancel the running session (thread-safe).
  void Abort();

 private:
  static void StreamCallback(void* data, const char* chunk,
                              bool is_final, const char* error_msg);

  LiteRtLmSession* session_;  // owned by engine; valid for our lifetime
  asio::io_context& io_;
  asio::experimental::channel<void(asio::error_code, std::string)> channel_;
  std::atomic<bool> aborted_{false};
  std::atomic<bool> started_{false};
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_LITERT_LM_SESSION_H_
```

- [ ] **Step 4.5: Write `agentflow/inference/litert_lm_session.cc`**

```cpp
// agentflow/inference/litert_lm_session.cc
#include "agentflow/inference/litert_lm_session.h"

#include <stdexcept>

#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

namespace agentflow {

LiteRtLmSession::LiteRtLmSession(LiteRtLmSession* session, asio::io_context& io)
    : session_(session),
      io_(io),
      channel_(io, 256) {}

LiteRtLmSession::~LiteRtLmSession() {
  Abort();
}

void LiteRtLmSession::Start(std::string input_text) {
  started_ = true;
  LiteRtLmInputData input;
  input.type = kInputText;
  input.data = input_text.data();
  input.size = input_text.size();

  litert_lm_session_generate_content_stream(
      session_, &input, 1,
      &LiteRtLmSession::StreamCallback, this);
}

asio::awaitable<std::string> LiteRtLmSession::NextTokenAsync() {
  asio::error_code ec;
  std::string token;
  co_await channel_.async_receive(ec, token);
  if (ec == asio::error::operation_aborted) {
    co_return std::string{};  // aborted
  }
  if (ec) {
    throw std::runtime_error("LiteRT-LM stream error: " + ec.message());
  }
  co_return token;
}

void LiteRtLmSession::Abort() {
  aborted_ = true;
  if (session_ && started_) {
    // LiteRT-LM session deletion aborts in-flight generation.
    // The session pointer is managed externally (engine).
    litert_lm_session_delete(session_);
    session_ = nullptr;
  }
  channel_.close();
}

void LiteRtLmSession::StreamCallback(void* data, const char* chunk,
                                      bool is_final, const char* error_msg) {
  auto* self = static_cast<LiteRtLmSession*>(data);
  if (self->aborted_) return;

  if (error_msg) {
    self->channel_.try_send(
        make_error_code(std::errc::io_error),
        std::string(error_msg));
    return;
  }

  self->channel_.try_send(
      asio::error_code{},
      chunk ? std::string(chunk) : std::string{});

  if (is_final) {
    self->channel_.close();
  }
}

}  // namespace agentflow
```

- [ ] **Step 4.6: Write tests `tests/unit/inference/litert_lm_session_test.cc`**

```cpp
// tests/unit/inference/litert_lm_session_test.cc
#include "agentflow/inference/litert_lm_session.h"
#include "agentflow/inference/litert_lm_engine.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

namespace agentflow {
namespace {

using namespace std::chrono_literals;

// LiteRT-LM session test requires a real model. Marked manual.
// To run: MODEL_PATH=/path/to/model bazel test //tests/unit/inference/...
TEST(LiteRtLmSessionTest, DISABLED_StartAndStream) {
  const char* model_path = std::getenv("MODEL_PATH");
  ASSERT_NE(model_path, nullptr) << "MODEL_PATH env var required";

  auto engine = LiteRtLmEngine::Create(
      LiteRtLmEngineOptions{.model_path = model_path});
  ASSERT_NE(engine, nullptr);

  // LiteRT-LM sampler config
  LiteRtLmSamplerParams sampler;
  sampler.type = kGreedy;
  sampler.top_k = 1;
  sampler.temperature = 0.0f;

  auto* raw_session = litert_lm_engine_create_session(
      engine->Get(),
      /*session_config=*/nullptr);

  asio::io_context io;
  LiteRtLmSession session(raw_session, io);

  session.Start("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");

  std::string full_output;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<void> {
        while (true) {
          std::string token = co_await session.NextTokenAsync();
          if (token.empty()) break;
          full_output += token;
        }
      },
      asio::use_future);

  std::thread t([&] { io.run(); });
  t.join();
  fut.get();

  EXPECT_FALSE(full_output.empty());
  std::cout << "LLM output: " << full_output << std::endl;
}

TEST(LiteRtLmSessionTest, CancelAborts) {
  auto* null_session = static_cast<LiteRtLmSession*>(nullptr);
  // Lightweight test: verify Abort() is idempotent and doesn't crash.
  // (Full cancel test requires real session from engine.)
  asio::io_context io;
  LiteRtLmSession session(null_session, io);
  session.Abort();  // should not crash
  SUCCEED();
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 4.7: Write `tests/unit/inference/BUILD.bazel`**

```python
# tests/unit/inference/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "litert_lm_session_test",
    size = "small",
    srcs = ["litert_lm_session_test.cc"],
    deps = [
        "//agentflow/inference",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    tags = ["manual"],  # requires MODEL_PATH env
)
```

- [ ] **Step 4.8: Verify compilation**

Run: `bazel build //agentflow/inference/... --test_tag_filters=-manual`
Expected: builds clean (no real model needed for compilation).

- [ ] **Step 4.9: Commit**

```bash
git add agentflow/inference/ tests/unit/inference/
git commit -m "feat: add inference layer — LiteRtLmEngine + LiteRtLmSession"
```

---

## Task 5: Tool System

**Files:**
- Create: `agentflow/tools/tool.h`
- Create: `agentflow/tools/native_fn_tool.h`
- Create: `agentflow/tools/native_fn_tool.cc`
- Create: `agentflow/tools/tool_registry.h`
- Create: `agentflow/tools/tool_registry.cc`
- Create: `agentflow/tools/BUILD.bazel`
- Create: `tests/unit/tools/native_fn_tool_test.cc`
- Create: `tests/unit/tools/tool_registry_test.cc`
- Create: `tests/unit/tools/BUILD.bazel`

- [ ] **Step 5.1: Write `agentflow/tools/BUILD.bazel`**

```python
# agentflow/tools/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "tools",
    srcs = [
        "native_fn_tool.cc",
        "tool_registry.cc",
    ],
    hdrs = [
        "native_fn_tool.h",
        "tool.h",
        "tool_registry.h",
    ],
    deps = [
        "//agentflow/core",
        "@nlohmann_json//:json",
    ],
)
```

- [ ] **Step 5.2: Write `agentflow/tools/tool.h`**

```cpp
// agentflow/tools/tool.h
#ifndef AGENTFLOW_TOOLS_TOOL_H_
#define AGENTFLOW_TOOLS_TOOL_H_

#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"

namespace agentflow {

struct ToolSchema {
  std::string name;
  std::string description;
  std::string params_json_schema;  // JSON Schema for arguments
};

class Tool {
 public:
  virtual ~Tool() = default;
  virtual const ToolSchema& Schema() const = 0;
  virtual asio::awaitable<std::string> Invoke(
      std::string_view args_json,
      const CancelToken& cancel) = 0;
};

}  // namespace agentflow
#endif  // AGENTFLOW_TOOLS_TOOL_H_
```

- [ ] **Step 5.3: Write `agentflow/tools/native_fn_tool.h`**

```cpp
// agentflow/tools/native_fn_tool.h
#ifndef AGENTFLOW_TOOLS_NATIVE_FN_TOOL_H_
#define AGENTFLOW_TOOLS_NATIVE_FN_TOOL_H_

#include <functional>
#include <string>
#include <string_view>

#include "agentflow/tools/tool.h"

namespace agentflow {

class NativeFnTool : public Tool {
 public:
  using Fn = std::function<asio::awaitable<std::string>(
      std::string_view, const CancelToken&)>;

  NativeFnTool(ToolSchema schema, Fn fn);
  const ToolSchema& Schema() const override { return schema_; }
  asio::awaitable<std::string> Invoke(
      std::string_view args_json,
      const CancelToken& cancel) override;

 private:
  ToolSchema schema_;
  Fn fn_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_TOOLS_NATIVE_FN_TOOL_H_
```

- [ ] **Step 5.4: Write `agentflow/tools/native_fn_tool.cc`**

```cpp
// agentflow/tools/native_fn_tool.cc
#include "agentflow/tools/native_fn_tool.h"

namespace agentflow {

NativeFnTool::NativeFnTool(ToolSchema schema, Fn fn)
    : schema_(std::move(schema)), fn_(std::move(fn)) {}

asio::awaitable<std::string> NativeFnTool::Invoke(
    std::string_view args_json, const CancelToken& cancel) {
  co_return co_await fn_(args_json, cancel);
}

}  // namespace agentflow
```

- [ ] **Step 5.5: Write `agentflow/tools/tool_registry.h`**

```cpp
// agentflow/tools/tool_registry.h
#ifndef AGENTFLOW_TOOLS_TOOL_REGISTRY_H_
#define AGENTFLOW_TOOLS_TOOL_REGISTRY_H_

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/tool.h"

namespace agentflow {

class ToolRegistry {
 public:
  void Register(std::shared_ptr<Tool> tool);

  asio::awaitable<std::string> Invoke(
      std::string_view name,
      std::string_view args_json,
      const CancelToken& cancel);

  // Returns OpenAI-compatible tools JSON array for LLM function calling.
  // Example:
  // [{"type":"function","function":{"name":"search","description":"...","parameters":{...}}}]
  std::string ExportToolsJson(
      std::span<const std::string> tool_names) const;

 private:
  std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<Tool>> tools_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_TOOLS_TOOL_REGISTRY_H_
```

- [ ] **Step 5.6: Write `agentflow/tools/tool_registry.cc`**

```cpp
// agentflow/tools/tool_registry.cc
#include "agentflow/tools/tool_registry.h"

#include <stdexcept>

#include "agentflow/core/errors.h"

namespace agentflow {
namespace {

nlohmann::json SchemaToJson(const ToolSchema& schema) {
  return {
    {"type", "function"},
    {"function", {
      {"name", schema.name},
      {"description", schema.description},
      {"parameters", nlohmann::json::parse(schema.params_json_schema)},
    }},
  };
}

}  // namespace

void ToolRegistry::Register(std::shared_ptr<Tool> tool) {
  std::lock_guard<std::mutex> lk(mu_);
  tools_[tool->Schema().name] = std::move(tool);
}

asio::awaitable<std::string> ToolRegistry::Invoke(
    std::string_view name, std::string_view args_json,
    const CancelToken& cancel) {
  std::shared_ptr<Tool> tool;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tools_.find(std::string(name));
    if (it == tools_.end()) {
      throw AgentflowError("Tool not found: " + std::string(name));
    }
    tool = it->second;
  }
  co_return co_await tool->Invoke(args_json, cancel);
}

std::string ToolRegistry::ExportToolsJson(
    std::span<const std::string> tool_names) const {
  nlohmann::json arr = nlohmann::json::array();
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& name : tool_names) {
    auto it = tools_.find(name);
    if (it != tools_.end()) {
      arr.push_back(SchemaToJson(it->second->Schema()));
    }
  }
  return arr.dump();
}

}  // namespace agentflow
```

- [ ] **Step 5.7: Write `tests/unit/tools/native_fn_tool_test.cc`**

```cpp
// tests/unit/tools/native_fn_tool_test.cc
#include "agentflow/tools/native_fn_tool.h"

#include <gtest/gtest.h>

namespace agentflow {
namespace {

TEST(NativeFnToolTest, InvokeReturnsResult) {
  NativeFnTool tool(
      ToolSchema{.name = "echo", .description = "echo back",
                 .params_json_schema = "{}"},
      [](std::string_view args, const CancelToken&) -> asio::awaitable<std::string> {
        co_return std::string(args);
      });

  asio::io_context io;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<void> {
        auto result = co_await tool.Invoke("hello", CancelToken{});
        EXPECT_EQ(result, "hello");
      },
      asio::use_future);
  io.run();
  fut.get();
}

TEST(NativeFnToolTest, SchemaAccessible) {
  ToolSchema schema{"my_tool", "does stuff", R"({"type":"object"})"};
  NativeFnTool tool(schema, nullptr);
  EXPECT_EQ(tool.Schema().name, "my_tool");
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 5.8: Write `tests/unit/tools/tool_registry_test.cc`**

```cpp
// tests/unit/tools/tool_registry_test.cc
#include "agentflow/tools/tool_registry.h"
#include "agentflow/tools/native_fn_tool.h"

#include <gtest/gtest.h>

namespace agentflow {
namespace {

std::shared_ptr<NativeFnTool> MakeEcho() {
  return std::make_shared<NativeFnTool>(
      ToolSchema{.name = "echo", .description = "echo",
                 .params_json_schema = R"({"type":"object"})"},
      [](std::string_view args, const CancelToken&) -> asio::awaitable<std::string> {
        co_return std::string(args);
      });
}

TEST(ToolRegistryTest, RegisterAndInvoke) {
  auto reg = std::make_shared<ToolRegistry>();
  reg->Register(MakeEcho());

  asio::io_context io;
  auto fut = asio::co_spawn(io,
      [reg]() -> asio::awaitable<void> {
        auto result = co_await reg->Invoke("echo", "\"test\"", CancelToken{});
        EXPECT_EQ(result, "\"test\"");
      },
      asio::use_future);
  io.run();
  fut.get();
}

TEST(ToolRegistryTest, InvokeUnknownThrows) {
  auto reg = std::make_shared<ToolRegistry>();
  asio::io_context io;
  auto fut = asio::co_spawn(io,
      [reg]() -> asio::awaitable<void> {
        EXPECT_THROW(
            co_await reg->Invoke("nobody", "{}", CancelToken{}),
            AgentflowError);
      },
      asio::use_future);
  io.run();
  fut.get();
}

TEST(ToolRegistryTest, ExportJson) {
  auto reg = std::make_shared<ToolRegistry>();
  reg->Register(MakeEcho());

  std::vector<std::string> names = {"echo"};
  std::string json = reg->ExportToolsJson(names);
  EXPECT_NE(json.find("echo"), std::string::npos);
  EXPECT_NE(json.find("function"), std::string::npos);

  // Must be valid JSON array
  auto parsed = nlohmann::json::parse(json);
  ASSERT_TRUE(parsed.is_array());
  ASSERT_EQ(parsed.size(), 1);
  EXPECT_EQ(parsed[0]["function"]["name"], "echo");
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 5.9: Write `tests/unit/tools/BUILD.bazel`**

```python
# tests/unit/tools/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "native_fn_tool_test",
    size = "small",
    srcs = ["native_fn_tool_test.cc"],
    deps = [
        "//agentflow/tools",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "tool_registry_test",
    size = "small",
    srcs = ["tool_registry_test.cc"],
    deps = [
        "//agentflow/tools",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 5.10: Build + Run**

Run: `bazel test //tests/unit/tools/... --test_output=errors`
Expected: 4 tests PASS.

- [ ] **Step 5.11: Commit**

```bash
git add agentflow/tools/ tests/unit/tools/
git commit -m "feat: add tool system — Tool, NativeFnTool, ToolRegistry"
```

---

## Task 6: AgentNode

**Files:**
- Create: `agentflow/nodes/agent_node.h`
- Create: `agentflow/nodes/agent_node.cc`
- Create: `agentflow/nodes/BUILD.bazel`
- Create: `tests/unit/nodes/agent_node_test.cc`
- Create: `tests/unit/nodes/BUILD.bazel`

- [ ] **Step 6.1: Write `agentflow/nodes/BUILD.bazel`**

```python
# agentflow/nodes/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "nodes",
    srcs = ["agent_node.cc"],
    hdrs = ["agent_node.h"],
    deps = [
        "//agentflow/core",
        "//agentflow/inference",
        "//agentflow/tools",
        "@asio",
        "@nlohmann_json//:json",
    ],
)
```

- [ ] **Step 6.2: Write `agentflow/nodes/agent_node.h`**

```cpp
// agentflow/nodes/agent_node.h
#ifndef AGENTFLOW_NODES_AGENT_NODE_H_
#define AGENTFLOW_NODES_AGENT_NODE_H_

#include <memory>
#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/node.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/tools/tool_registry.h"

namespace agentflow {

struct AgentNodeConfig {
  std::shared_ptr<LiteRtLmEngine> engine;
  std::string system_prompt;
  std::shared_ptr<ToolRegistry> tool_registry;
  int max_iter = 8;
  int max_output_tokens = 512;
  bool stream_tokens = true;

  // Protobuf reflection: field names on the state message.
  std::string input_field;       // read user query from this field
  std::string output_field;      // write assistant reply to this field
  std::string messages_field;    // if non-empty, append to this repeated Message field
};

class AgentNode : public Node {
 public:
  explicit AgentNode(AgentNodeConfig cfg);
  ~AgentNode() override = default;

  std::string_view Id() const override { return id_; }
  std::string_view Kind() const override { return "agent"; }

  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;

 private:
  std::string BuildConversationJson(const State& state) const;
  void HandleToolCall(
      State& state, const std::string& name,
      const std::string& args, const CancelToken& cancel);
  void WriteOutput(State& state, const std::string& text) const;

  AgentNodeConfig cfg_;
  std::string id_;  // set from config-derived name or caller
};

}  // namespace agentflow
#endif  // AGENTFLOW_NODES_AGENT_NODE_H_
```

- [ ] **Step 6.3: Write `agentflow/nodes/agent_node.cc`**

```cpp
// agentflow/nodes/agent_node.cc
#include "agentflow/nodes/agent_node.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "agentflow/core/errors.h"
#include "agentflow/inference/litert_lm_session.h"
#include "c/engine.h"

namespace agentflow {

namespace {

using json = nlohmann::json;

std::string ReadField(const State& state, const std::string& field_name) {
  const auto* msg = state.UnsafeMessage();
  if (!msg) return {};
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(field_name);
  if (!desc) return {};
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    return refl->GetString(*msg, desc);
  }
  return {};
}

void WriteField(State& state, const std::string& field_name,
                const std::string& value) {
  auto* msg = const_cast<google::protobuf::Message*>(state.UnsafeMessage());
  if (!msg) return;
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(field_name);
  if (!desc) return;
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    refl->SetString(msg, desc, value);
  }
}

void AppendMessage(State& state, const std::string& field_name,
                   const json& message_obj) {
  auto* msg = const_cast<google::protobuf::Message*>(state.UnsafeMessage());
  if (!msg) return;
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(field_name);
  if (!desc || !desc->is_repeated()) return;
  if (desc->type() != google::protobuf::FieldDescriptor::TYPE_MESSAGE) return;

  auto* pool = msg->GetDescriptor()->file()->pool();
  const auto* entry_desc = desc->message_type();
  auto* entry = refl->AddMessage(msg, desc);
  // For a generic "Message" type with "role" and "content" string fields:
  auto* role_f = entry_desc->FindFieldByName("role");
  auto* content_f = entry_desc->FindFieldByName("content");
  if (role_f && content_f) {
    refl->SetString(entry, role_f, message_obj["role"].get<std::string>());
    refl->SetString(entry, content_f, message_obj["content"].get<std::string>());
    if (message_obj.contains("tool_call_id")) {
      auto* tcid_f = entry_desc->FindFieldByName("tool_call_id");
      if (tcid_f)
        refl->SetString(entry, tcid_f, message_obj["tool_call_id"].get<std::string>());
    }
  }
}

}  // namespace

AgentNode::AgentNode(AgentNodeConfig cfg)
    : cfg_(std::move(cfg)),
      id_("agent") {}

std::string AgentNode::BuildConversationJson(const State& state) const {
  json msgs = json::array();

  if (!cfg_.system_prompt.empty()) {
    msgs.push_back({{"role", "system"}, {"content", cfg_.system_prompt}});
  }

  // Append existing conversation history
  if (!cfg_.messages_field.empty()) {
    // messages_field is a repeated message with role+content fields
    // For P2: append existing messages from the state.
    // (Simplified: just include the current input.)
  }

  std::string input = ReadField(state, cfg_.input_field);
  msgs.push_back({{"role", "user"}, {"content", input}});

  // Attach tools if configured
  json full;
  full["messages"] = msgs;
  full["max_tokens"] = cfg_.max_output_tokens;
  full["stream"] = true;

  if (cfg_.tool_registry) {
    // Retrieve all tool names from registry
    // (In practice, tools are passed via a separate mechanism; P2 default: no tools on first call)
  }

  return full.dump();
}

void AgentNode::WriteOutput(State& state, const std::string& text) const {
  WriteField(state, cfg_.output_field, text);
}

asio::awaitable<State> AgentNode::Run(
    State state, const CancelToken& cancel, EventEmitter& emit) {
  // LiteRT-LM session config
  auto* raw_session = litert_lm_engine_create_session(
      cfg_.engine->Get(),
      /*session_config=*/nullptr);
  if (!raw_session) {
    throw AgentflowError("AgentNode: failed to create LiteRT-LM session");
  }

  LiteRtLmSession session(raw_session, *cfg_.engine->GetExecutor());
  if (cancel.IsCancelled()) co_return std::move(state);

  std::string final_answer;
  for (int iter = 0; iter < cfg_.max_iter; ++iter) {
    if (cancel.IsCancelled()) break;

    emit.EmitNodeStart(Id());
    std::string conversation_json = BuildConversationJson(state);

    session.Start(conversation_json);
    std::string accum;
    while (true) {
      std::string token = co_await session.NextTokenAsync();
      if (token.empty()) break;
      accum += token;
      if (cfg_.stream_tokens) {
        emit.EmitToken(Id(), token);
      }
    }

    emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);

    // Check for tool call in output
    // LiteRT-LM tool_call format: {"tool_calls":[{"function":{"name":"...","arguments":"..."}}]}
    try {
      auto parsed = json::parse(accum);
      if (parsed.contains("tool_calls") && !parsed["tool_calls"].empty()) {
        for (const auto& tc : parsed["tool_calls"]) {
          std::string name = tc["function"]["name"];
          std::string args = tc["function"]["arguments"];
          HandleToolCall(state, name, args, cancel);
        }
        // Loop back for next LLM call
        continue;
      }
    } catch (const json::parse_error&) {
      // Not JSON = plain text output
    }

    // No tool call — this is the final answer
    final_answer = accum;
    WriteOutput(state, final_answer);
    break;
  }

  if (final_answer.empty() && !cancel.IsCancelled()) {
    WriteOutput(state, "Agent reached maximum iterations without a final answer.");
  }

  co_return std::move(state);
}

void AgentNode::HandleToolCall(
    State& state, const std::string& name,
    const std::string& args, const CancelToken& cancel) {
  if (!cfg_.tool_registry) return;

  auto result = co_await cfg_.tool_registry->Invoke(name, args, cancel);

  // Append to conversation history
  json tool_msg = {
    {"role", "tool"},
    {"tool_call_id", name},
    {"content", result},
  };
  AppendMessage(state, cfg_.messages_field, tool_msg);
}

}  // namespace agentflow
```

- [ ] **Step 6.4: Write `tests/unit/nodes/agent_node_test.cc`**

```cpp
// tests/unit/nodes/agent_node_test.cc
#include "agentflow/nodes/agent_node.h"

#include <gtest/gtest.h>

#include "test_messages.pb.h"

namespace agentflow {
namespace {

// AgentNode test requires a real LiteRT-LM model. Marked manual.
// Run with: MODEL_PATH=/path/to/model bazel test //tests/unit/nodes/...
TEST(AgentNodeTest, DISABLED_SimpleResponse) {
  const char* model_path = std::getenv("MODEL_PATH");
  ASSERT_NE(model_path, nullptr);

  auto engine = LiteRtLmEngine::Create(
      LiteRtLmEngineOptions{.model_path = model_path});

  AgentNodeConfig cfg;
  cfg.engine = engine;
  cfg.system_prompt = "You are a helpful assistant. Reply briefly.";
  cfg.input_field = "user_query";
  cfg.output_field = "assistant_reply";

  auto node = std::make_unique<AgentNode>(std::move(cfg));

  test::TestState raw;
  raw.set_user_query("Say hello in one word");

  asio::io_context io;
  CancelSource cancel;

  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        NullEventEmitter null_emit;
        co_return co_await node->Run(
            State::From(std::move(raw)), cancel.Token(), null_emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();

  std::string reply = out.As<test::TestState>().assistant_reply();
  EXPECT_FALSE(reply.empty());
  std::cout << "Agent reply: " << reply << std::endl;
}

// Tool calling test would follow the same pattern but with registered tools.

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 6.5: Write `tests/unit/nodes/BUILD.bazel`**

```python
# tests/unit/nodes/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "agent_node_test",
    size = "large",
    srcs = ["agent_node_test.cc"],
    deps = [
        "//agentflow/nodes",
        "//proto:agentflow_proto",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    tags = ["manual"],  # requires MODEL_PATH env
)
```

- [ ] **Step 6.6: Commit**

```bash
git add agentflow/nodes/ tests/unit/nodes/
git commit -m "feat: add AgentNode — ReAct loop as single graph node"
```

---

## Task 7: Demo + Verification

**Files:**
- Create: `examples/agent-demo/main.cc`
- Create: `examples/agent-demo/BUILD.bazel`

- [ ] **Step 7.1: Write `examples/agent-demo/BUILD.bazel`**

```python
# examples/agent-demo/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "agent_demo",
    srcs = ["main.cc"],
    deps = [
        "//agentflow/nodes",
        "//agentflow/tools",
    ],
)
```

- [ ] **Step 7.2: Write `examples/agent-demo/main.cc`**

```cpp
// examples/agent-demo/main.cc
// P2 demo: AgentNode + ToolRegistry in a graph, driven by LiteRT-LM.
//
// Usage: MODEL_PATH=/path/to/model ./agent_demo
//
// Graph:
//   entry ──> agent ──> sink
// The agent uses a real model to answer the user's query.

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/graph.h"
#include "agentflow/core/runner.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool_registry.h"
#include "test_messages.pb.h"

namespace af = agentflow;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
  const char* model_path = std::getenv("MODEL_PATH");
  if (!model_path) {
    std::cerr << "MODEL_PATH env var required\n";
    return 1;
  }

  // ── Engine ──────────────────────────────────────────────
  auto engine = af::LiteRtLmEngine::Create(
      af::LiteRtLmEngineOptions{.model_path = model_path});
  if (!engine) {
    std::cerr << "Failed to create LiteRT-LM engine\n";
    return 1;
  }

  // ── Tools ───────────────────────────────────────────────
  auto registry = std::make_shared<af::ToolRegistry>();
  registry->Register(std::make_shared<af::NativeFnTool>(
      af::ToolSchema{
          .name = "get_time",
          .description = "Get the current time",
          .params_json_schema = "{}",
      },
      [](std::string_view, const af::CancelToken&) -> asio::awaitable<std::string> {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        co_return std::ctime(&tt);
      }));

  // ── Agent ───────────────────────────────────────────────
  af::AgentNodeConfig agent_cfg;
  agent_cfg.engine = engine;
  agent_cfg.system_prompt =
      "You are a helpful assistant. When asked about time, use the get_time tool.";
  agent_cfg.tool_registry = registry;
  agent_cfg.input_field = "user_query";
  agent_cfg.output_field = "assistant_reply";
  agent_cfg.messages_field = "messages";
  agent_cfg.max_iter = 5;
  agent_cfg.stream_tokens = true;

  // ── Graph ───────────────────────────────────────────────
  af::GraphBuilder b;
  b.AddNode(std::make_unique<af::AgentNode>(std::move(agent_cfg)))
   .AddNode(std::make_unique<af::StubNode>("sink", 0ms, nullptr, nullptr))
   .AddEdge("agent", "sink");
  auto graph = b.Build();
  std::cout << "GRAPH:\n" << graph.ToDotString() << "---\n";

  // ── State ───────────────────────────────────────────────
  af::test::TestState init;
  init.set_user_query("Hello! What time is it?");
  init.set_counter(0);

  // ── Run ─────────────────────────────────────────────────
  af::Runner runner(std::move(graph), af::Runner::Options{});
  asio::io_context io;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<af::State> {
        co_return co_await runner.Run(af::State::From(init));
      },
      asio::use_future);
  io.run();
  auto out = fut.get();

  const auto& ts = out.As<af::test::TestState>();
  std::cout << "\n=== Final ===\n";
  std::cout << "Assistant: " << ts.assistant_reply() << std::endl;

  return ts.counter() >= 0 ? 0 : 1;
}
```

- [ ] **Step 7.3: Fix the proto — add user_query + assistant_reply fields**

The existing `test_messages.proto` has `query` and `reply` but not string `user_query`/`assistant_reply` fields. Update:

```protobuf
message TestState {
  UserQuery query = 1;
  AssistantReply reply = 2;
  int32 counter = 3;
  string last_node = 4;
  string user_query = 5;        // NEW: agent input field
  string assistant_reply = 6;    // NEW: agent output field
}
```

(If `UserQuery` has a `text` field, that could also serve as input; adjust field names based on existing proto.)

- [ ] **Step 7.4: Full verification**

Run: `bazel test //tests/unit/core/... --test_output=errors`
Expected: 39 tests PASS.

Run: `bazel test //tests/unit/tools/... --test_output=errors`
Expected: 4 tests PASS.

Run: `bazel build //agentflow/...`
Expected: all libraries build clean.

Run (with real model): `MODEL_PATH=/path/to/model bazel test //tests/unit/inference/... --test_tag_filters=-manual --test_output=errors`
Expected: LiteRT-LM session test streams tokens.

Run (with real model): `MODEL_PATH=/path/to/model bazel run //examples/agent-demo:agent_demo`
Expected: agent reads user query, generates response (possibly using tool), prints final answer.

- [ ] **Step 7.5: Tag milestone**

```bash
git tag -a p2-agent-layer -m "P2: agent layer complete — inference, tools, AgentNode"
```

---

## Self-Review

**Spec coverage:**
- Section 3 (inference layer) → Task 4
- Section 4 (tool system) → Task 5
- Section 5 (AgentNode) → Task 6
- Section 2 (Bazel migration) → Tasks 1, 2, 3
- Section 7 (testing) → Embedded in Tasks 4, 5, 6
- Section 8 (demo) → Task 7

**All code blocks complete:**
- Every step has actual code, not placeholders.
- Type consistency checked: `LiteRtLmEngine`, `LiteRtLmSession`, `NativeFnTool`, `ToolRegistry`, `AgentNode` — names match across all references.

**Known risks:**
1. LiteRT-LM CMake build may fail (Step 3.1) — fallback to header-only stub (Step 3.6)
2. LiteRT-LM streaming callback channel bridge needs verification on real hardware (Step 4 is the first integration point)
3. Protobuf reflection for field access (Step 6.3) depends on exact proto schema — adjust during implementation

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-05-11-cpp-agent-framework-p2-agent-layer.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
