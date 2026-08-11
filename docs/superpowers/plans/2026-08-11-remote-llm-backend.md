# Remote LLM Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make an OpenAI-compatible remote LLM endpoint a first-class inference backend, interchangeable with on-device LiteRT-LM inside a single workflow.

**Architecture:** Promote the `SendFn`/`ConversationFactory` contract that already exists in `agentflow/workflow/sub_agent_runtime.h` into a public `IChatBackend`/`IConversation` seam under `agentflow/inference/`. Write two implementations behind it: `LiteRtLmChatBackend` (today's engine, behaviour unchanged) and `OpenAiChatBackend` (HTTPS + SSE). Backend instances are constructed by the host and selected per agent by logical name; credentials never enter the workflow spec.

**Tech Stack:** C++20, Bazel (bzlmod), asio (standalone, coroutines), BoringSSL, nlohmann_json, Abseil (`absl::Status`/`StatusOr`), GoogleTest.

**Design spec:** `docs/superpowers/specs/2026-08-11-remote-llm-backend-design.md`

## Global Constraints

- **C++20.** `.bazelrc` sets `--cxxopt=-std=c++20`. Coroutines (`asio::awaitable`) are used throughout.
- **`--features=layering_check` is enabled.** Every header you `#include` must come from a target listed in that rule's `deps`. Adding an include without adding the dep is a build error, not a warning.
- **`ANDROID_NDK_HOME` must be exported for every Bazel command**, even host-only builds — the NDK repo extension is instantiated unconditionally and fails analysis without it:
  ```bash
  export ANDROID_NDK_HOME=$HOME/android-ndk/android-ndk-r26d
  ```
- **`third_party/litert_lm/lib/` is gitignored** (large machine-built artifacts) and therefore absent in a fresh worktree. Link it before the first build:
  ```bash
  ln -sfn /home/wayne/tools/zen/third_party/litert_lm/lib third_party/litert_lm/lib
  ```
- **No new third-party dependencies.** Only asio, BoringSSL, nlohmann_json, Abseil, GoogleTest — all already in `MODULE.bazel`. Do not add libcurl, cpp-httplib, or OpenSSL.
- **Credentials never leave host code.** No API key, `base_url`, or provider model name may appear in a workflow spec, a checkpoint, a trace event, an error message, or a log line.
- **The canonical message shape is LiteRT-LM's existing shape.** Do not invent a new one; the remote implementation adapts to it.
- Protobuf is pinned to v31.1 via `git_override` in `MODULE.bazel`. Do not change it.

### Shorthand used in every test step

```bash
export ANDROID_NDK_HOME=$HOME/android-ndk/android-ndk-r26d
```
Assume this is exported in your shell. Every `bazel` command below relies on it.

### Established baseline (verified 2026-08-11)

```
//tests/unit/nodes:llm_node_test                PASSED in 0.2s
//tests/unit/workflow:sub_agent_runtime_test    PASSED in 0.1s
```

These two must stay green through **every** task. `sub_agent_runtime_test` is the regression gate for this plan's central premise: the `SendFn` contract does not change.

---

## File Structure

**New — the seam (no LiteRT-LM, no HTTP dependency):**

| File | Responsibility |
|---|---|
| `agentflow/inference/chat_backend.h` | `TokenSink`, `ChatConversationOptions`, `IConversation`, `IChatBackend`. Pure interface. |
| `agentflow/inference/canonical_message.h/.cc` | Pure helpers on the canonical shape: assemble a canonical assistant JSON from LiteRT stream envelopes; extract assistant text. No I/O. |

**New — LiteRT-LM behind the seam:**

| File | Responsibility |
|---|---|
| `agentflow/inference/litert_lm_chat_backend.h/.cc` | `IChatBackend` over `LiteRtLmEngine`; owns the stream-envelope handling moved out of `AgentNode`. |

**New — HTTP layer (depends only on asio + BoringSSL):**

| File | Responsibility |
|---|---|
| `agentflow/net/http_client.h` | `HttpRequest`, `SseHandler`, `IHttpClient` interface. No implementation. |
| `agentflow/net/http_parse.h/.cc` | Pure parsers: `ParseUrl`, `ParseResponseHead`, `ChunkedDecoder`, `SseFramer`. No sockets. |
| `agentflow/net/https_client.h/.cc` | `HttpsClient : IHttpClient` — sockets + TLS. The only file touching the network. |

**New — OpenAI adapter:**

| File | Responsibility |
|---|---|
| `agentflow/inference/openai/message_map.h/.cc` | Pure canonical↔OpenAI conversion. No I/O. |
| `agentflow/inference/openai/stream_accumulator.h/.cc` | Pure SSE-delta accumulation (text + fragmented tool_calls). No I/O. |
| `agentflow/inference/openai/openai_chat_backend.h/.cc` | `IChatBackend` wiring: request building, retry policy, cancellation. |

**New — test support:**

| File | Responsibility |
|---|---|
| `tests/support/fake_chat_backend.h` | `FakeChatBackend`/`FakeConversation` — scripted canonical responses. Used by `AgentNode`, `LlmNode`, workflow tests. |
| `tests/support/fake_http_client.h` | `FakeHttpClient` — replays canned SSE frames and canned non-streaming bodies. |

**Modified:**

| File | Change |
|---|---|
| `agentflow/inference/litert_lm_conversation.h` | `LiteRtLmConversationOptions` becomes an alias of `ChatConversationOptions`. |
| `agentflow/nodes/agent_node.h/.cc` | `engine` → `backend`; two run branches collapse to one; tool-call `id` passthrough. |
| `agentflow/nodes/llm_node.h/.cc` | `engine` → `backend`. |
| `agentflow/workflow/sub_agent_runtime.h/.cc` | `TokenSink` becomes an alias; `DefaultConversationFactory` takes a backend. |
| `agentflow/workflow/workflow_runner.h/.cc` | `engine` → `backend` + `backends` map. |
| `agentflow/workflow/workflow_loader.cc` | Validate `ModelSpec.backend` against the registered map. |
| `proto/workflow_spec.proto` | `ModelSpec` gains `string backend = 3;`. |
| `tests/unit/nodes/agent_node_test.cc` | Un-skip the three cases using `FakeChatBackend`. |

---

## Task 1: TLS spike — decide `HttpsClient`'s internals

Spec §4.1. This is a decision-producing task, deliberately first: it blocks nothing else, and its outcome selects between two implementations in Task 9.

**Files:**
- Create: `agentflow/net/BUILD.bazel`
- Create: `tests/unit/net/BUILD.bazel`
- Create: `tests/unit/net/tls_probe_test.cc`

**Interfaces:**
- Consumes: nothing.
- Produces: a recorded decision — `asio::ssl` viable, or fall back to manual BoringSSL memory BIOs. Task 9 reads this decision. No headers are produced for other tasks.

- [ ] **Step 1: Create the package for the net layer**

Create `agentflow/net/BUILD.bazel`:

```python
# agentflow/net/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])
```

- [ ] **Step 2: Write the probe test**

Create `tests/unit/net/tls_probe_test.cc`:

```cpp
// tests/unit/net/tls_probe_test.cc
//
// Decides how HttpsClient talks TLS (design spec §4.1).
//
// asio::ssl is written against the OpenSSL API. BoringSSL reports
// OPENSSL_VERSION_NUMBER as 1.1.1 so asio takes its 1.1.1 code path, but
// neither project promises compatibility. If this file compiles, links and
// passes, HttpsClient uses asio::ssl. If it does not COMPILE, HttpsClient
// drives BoringSSL manually with memory BIOs instead (Task 8, Variant B).
//
// A compile failure here is a legitimate, expected outcome — not a defect
// to debug. Record which variant applies and move on.

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>

#include <gtest/gtest.h>

TEST(TlsProbe, AsioSslContextConstructsOverBoringSsl) {
  asio::io_context io;
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  ctx.set_verify_mode(asio::ssl::verify_peer);
  asio::ssl::stream<asio::ip::tcp::socket> stream(io, ctx);
  // Reaching here means construction and linkage both work.
  SUCCEED();
}

TEST(TlsProbe, AsioSslLoadsSystemCaBundle) {
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  asio::error_code ec;
  ctx.load_verify_file("/etc/ssl/certs/ca-certificates.crt", ec);
  if (ec) GTEST_SKIP() << "no system CA bundle at the expected path: " << ec.message();
  SUCCEED();
}

TEST(TlsProbe, AsioSslSetsSniHostname) {
  asio::io_context io;
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  asio::ssl::stream<asio::ip::tcp::socket> stream(io, ctx);
  // SNI is mandatory for every cloud endpoint. SSL_set_tlsext_host_name is a
  // macro over SSL_ctrl in both OpenSSL and BoringSSL; verify it resolves.
  ASSERT_EQ(1, SSL_set_tlsext_host_name(stream.native_handle(), "api.openai.com"));
}
```

- [ ] **Step 3: Declare the test target**

Create `tests/unit/net/BUILD.bazel`:

```python
# tests/unit/net/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "tls_probe_test",
    size = "small",
    srcs = ["tls_probe_test.cc"],
    deps = [
        "@asio",
        "@boringssl//:crypto",
        "@boringssl//:ssl",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 4: Run the probe and record the outcome**

Run: `bazel test //tests/unit/net:tls_probe_test --test_output=all`

Three possible outcomes, all valid:

| Outcome | Meaning | Task 8 variant |
|---|---|---|
| PASS | asio::ssl works over BoringSSL | **Task 9 Variant A** (asio::ssl) |
| Compile/link error | API incompatibility | **Task 9 Variant B** (manual memory BIO) |
| `AsioSslLoadsSystemCaBundle` skipped, others pass | fine — CA path is configurable anyway | **Task 9 Variant A** |

If it fails to compile, **do not attempt to patch asio.** Delete `tls_probe_test.cc` and its target, and note Variant B in the commit message.

- [ ] **Step 5: Commit**

```bash
git add agentflow/net/BUILD.bazel tests/unit/net/
git commit -m "test(net): TLS spike — decide asio::ssl vs manual BoringSSL BIO

Records which TLS integration HttpsClient will use (design spec 4.1).
Outcome: <PASS: asio::ssl / FAILED TO COMPILE: manual memory BIO>."
```

---

## Task 2: The `IChatBackend` seam

Spec §3. Interfaces only — no implementation, no behaviour change anywhere.

**Files:**
- Create: `agentflow/inference/chat_backend.h`
- Create: `tests/support/BUILD.bazel`
- Create: `tests/support/fake_chat_backend.h`
- Create: `tests/unit/inference/chat_backend_test.cc`
- Modify: `agentflow/inference/BUILD.bazel`
- Modify: `agentflow/inference/litert_lm_conversation.h` (options become an alias)
- Modify: `tests/unit/inference/BUILD.bazel`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `agentflow::TokenSink` = `std::function<void(std::string_view)>`
  - `agentflow::ChatConversationOptions{system_message_json, tools_json, messages_json, max_output_tokens, constrained_tool_calls}`
  - `agentflow::IConversation::SendAsync(std::string, const TokenSink&, const CancelToken&) -> asio::awaitable<absl::StatusOr<std::string>>`, `IConversation::Cancel()`
  - `agentflow::IChatBackend::CreateConversation(ChatConversationOptions) -> std::shared_ptr<IConversation>`, `IChatBackend::Describe() -> std::string_view`
  - `agentflow::LiteRtLmConversationOptions` is now an **alias** of `ChatConversationOptions` — every existing call site keeps compiling.
  - Test target `//tests/support:fake_chat_backend` providing `agentflow::testing::FakeChatBackend`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/inference/chat_backend_test.cc`:

```cpp
// tests/unit/inference/chat_backend_test.cc
#include "agentflow/inference/chat_backend.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "tests/support/fake_chat_backend.h"

namespace agentflow {
namespace {

TEST(ChatBackendTest, FakeBackendReturnsScriptedCanonicalResponse) {
  asio::io_context io;
  testing::FakeChatBackend backend({R"({"role":"assistant","content":[{"type":"text","text":"hi"}]})"});

  auto conv = backend.CreateConversation(ChatConversationOptions{});
  ASSERT_NE(conv, nullptr);

  std::string got;
  CancelSource cancel_src;
  const CancelToken cancel = cancel_src.Token();
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    auto r = co_await conv->SendAsync(R"({"role":"user","content":[]})",
                                       TokenSink{}, cancel);
    ASSERT_TRUE(r.ok());
    got = *r;
  }, asio::detached);
  io.run();

  EXPECT_EQ(got,
            R"({"role":"assistant","content":[{"type":"text","text":"hi"}]})");
}

TEST(ChatBackendTest, FakeBackendDeliversTextDeltasToTokenSink) {
  asio::io_context io;
  testing::FakeChatBackend backend(
      {R"({"role":"assistant","content":[{"type":"text","text":"ab"}]})"});
  backend.set_deltas({"a", "b"});

  auto conv = backend.CreateConversation(ChatConversationOptions{});
  std::vector<std::string> seen;
  CancelSource cancel_src;
  const CancelToken cancel = cancel_src.Token();

  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    auto r = co_await conv->SendAsync(
        R"({"role":"user","content":[]})",
        [&](std::string_view d) { seen.emplace_back(d); }, cancel);
    ASSERT_TRUE(r.ok());
  }, asio::detached);
  io.run();

  EXPECT_EQ(seen, (std::vector<std::string>{"a", "b"}));
}

TEST(ChatBackendTest, DescribeCarriesNoCredentials) {
  testing::FakeChatBackend backend({});
  EXPECT_EQ(backend.Describe(), "fake");
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test //tests/unit/inference:chat_backend_test`
Expected: FAIL — `agentflow/inference/chat_backend.h` does not exist.

- [ ] **Step 3: Write the interface header**

Create `agentflow/inference/chat_backend.h`:

```cpp
// agentflow/inference/chat_backend.h
#ifndef AGENTFLOW_INFERENCE_CHAT_BACKEND_H_
#define AGENTFLOW_INFERENCE_CHAT_BACKEND_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"

namespace agentflow {

// One text delta from the model. Same contract as the TokenSink that
// SubAgentRuntime already uses.
using TokenSink = std::function<void(std::string_view delta)>;

struct ChatConversationOptions {
  // A bare content ARRAY, not a {role,content} object:
  //   [{"type":"text","text":"You are ..."}]
  // LiteRT-LM wraps it into {role:system, content:<this>} itself; see
  // AgentNode::BuildSystemMessageJson. Empty means no system message.
  std::string system_message_json;

  // OpenAI tools shape, which is what AgentNode::BuildToolsJson already
  // emits: [{"type":"function","function":{name,description,parameters}}].
  std::string tools_json = "[]";

  // Initial history, canonical shape.
  std::string messages_json = "[]";

  int max_output_tokens = 1024;

  // LiteRT-only: attach an LLGuidance grammar derived from tools_json.
  // A remote backend cannot honour this; it emits a trace warning and runs
  // unconstrained rather than degrading silently (design spec §6).
  bool constrained_tool_calls = false;
};

// One multi-turn conversation. The implementation OWNS history: locally the
// engine does (so KV cache is reused across turns), remotely an internal
// messages array does. Callers never thread history between turns.
class IConversation {
 public:
  virtual ~IConversation() = default;

  // Sends one message JSON and awaits the full canonical assistant JSON:
  //   {"role":"assistant",
  //    "content":[{"type":"text","text":"..."}],
  //    "tool_calls":[{"id":"...","function":{"name":"...",
  //                                          "arguments":"{...}"}}]}
  // When `on_token` is set, each text delta is delivered as it arrives.
  // The return value is always canonical, whatever the backend.
  virtual asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) = 0;

  // Breaks the in-flight request. Safe to call from any thread.
  virtual void Cancel() = 0;
};

class IChatBackend {
 public:
  virtual ~IChatBackend() = default;

  virtual std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) = 0;

  // For traces and error messages, e.g. "litert-lm" or
  // "openai:deepseek-chat". MUST NOT contain credentials.
  virtual std::string_view Describe() const = 0;
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_CHAT_BACKEND_H_
```

- [ ] **Step 4: Give the seam its own Bazel target**

The existing `//agentflow/inference` target pulls in `//third_party/litert_lm:c_engine`. The seam must NOT, or every consumer would drag LiteRT-LM in. Split it.

Replace `agentflow/inference/BUILD.bazel` with:

```python
# agentflow/inference/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# The engine-agnostic seam. Deliberately does NOT depend on LiteRT-LM.
cc_library(
    name = "chat_backend",
    hdrs = ["chat_backend.h"],
    deps = [
        "//agentflow/core",
        "@abseil-cpp//absl/status:statusor",
        "@asio",
    ],
)

cc_library(
    name = "inference",
    srcs = [
        "litert_lm_conversation.cc",
        "litert_lm_engine.cc",
        "litert_lm_session.cc",
    ],
    hdrs = [
        "litert_lm_conversation.h",
        "litert_lm_engine.h",
        "litert_lm_session.h",
    ],
    deps = [
        ":chat_backend",
        "//third_party/litert_lm:c_engine",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@asio",
    ],
)
```

- [ ] **Step 5: Make `LiteRtLmConversationOptions` an alias**

This is what keeps every existing call site and the whole of
`sub_agent_runtime_test.cc` compiling untouched — the fields are already
identical.

In `agentflow/inference/litert_lm_conversation.h`, add the include near the
top:

```cpp
#include "agentflow/inference/chat_backend.h"
```

and replace the whole `struct LiteRtLmConversationOptions { ... };` block
with:

```cpp
// The LiteRT-specific name is retained as an alias so existing call sites
// and tests compile unchanged. The fields are identical; see
// agentflow/inference/chat_backend.h.
using LiteRtLmConversationOptions = ChatConversationOptions;
```

- [ ] **Step 6: Write the shared fake backend**

Create `tests/support/fake_chat_backend.h`:

```cpp
// tests/support/fake_chat_backend.h
//
// A scripted IChatBackend for tests: no engine, no network. Each SendAsync
// call pops the next canned canonical response. Used by chat_backend_test,
// agent_node_test and llm_node_test.
#ifndef TESTS_SUPPORT_FAKE_CHAT_BACKEND_H_
#define TESTS_SUPPORT_FAKE_CHAT_BACKEND_H_

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>

#include "agentflow/inference/chat_backend.h"

namespace agentflow::testing {

class FakeConversation : public IConversation {
 public:
  FakeConversation(std::deque<std::string> responses,
                    std::vector<std::string> deltas)
      : responses_(std::move(responses)), deltas_(std::move(deltas)) {}

  asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) override {
    sent_.push_back(std::move(message_json));
    if (cancel.IsCancelled()) {
      co_return absl::CancelledError("cancelled");
    }
    if (on_token) {
      for (const auto& d : deltas_) on_token(d);
    }
    if (responses_.empty()) {
      co_return absl::UnavailableError("fake: no scripted response left");
    }
    std::string r = std::move(responses_.front());
    responses_.pop_front();
    co_return r;
  }

  void Cancel() override { ++cancel_calls_; }

  // Messages this conversation received, in order. Lets a test assert what
  // AgentNode sent back after dispatching a tool.
  const std::vector<std::string>& sent() const { return sent_; }
  int cancel_calls() const { return cancel_calls_; }

 private:
  std::deque<std::string> responses_;
  std::vector<std::string> deltas_;
  std::vector<std::string> sent_;
  int cancel_calls_ = 0;
};

class FakeChatBackend : public IChatBackend {
 public:
  explicit FakeChatBackend(std::vector<std::string> responses)
      : responses_(responses.begin(), responses.end()) {}

  // Text deltas handed to the TokenSink on every SendAsync.
  void set_deltas(std::vector<std::string> deltas) {
    deltas_ = std::move(deltas);
  }

  std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) override {
    last_options_ = std::move(opts);
    auto conv = std::make_shared<FakeConversation>(responses_, deltas_);
    last_conversation_ = conv;
    return conv;
  }

  std::string_view Describe() const override { return "fake"; }

  // Options the node passed in — lets a test assert the system prompt and
  // tools JSON were built correctly.
  const ChatConversationOptions& last_options() const { return last_options_; }
  std::shared_ptr<FakeConversation> last_conversation() const {
    return last_conversation_.lock();
  }

 private:
  std::deque<std::string> responses_;
  std::vector<std::string> deltas_;
  ChatConversationOptions last_options_;
  std::weak_ptr<FakeConversation> last_conversation_;
};

}  // namespace agentflow::testing
#endif  // TESTS_SUPPORT_FAKE_CHAT_BACKEND_H_
```

Create `tests/support/BUILD.bazel`:

```python
# tests/support/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "fake_chat_backend",
    testonly = True,
    hdrs = ["fake_chat_backend.h"],
    deps = [
        "//agentflow/core",
        "//agentflow/inference:chat_backend",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@asio",
    ],
)
```

- [ ] **Step 7: Declare the test target**

Append to `tests/unit/inference/BUILD.bazel`:

```python
cc_test(
    name = "chat_backend_test",
    size = "small",
    srcs = ["chat_backend_test.cc"],
    deps = [
        "//agentflow/core",
        "//agentflow/inference:chat_backend",
        "//tests/support:fake_chat_backend",
        "@asio",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

Also change `tests/BUILD.bazel`'s visibility so `//tests/support` is usable —
replace its contents with:

```python
# tests/BUILD.bazel
package(default_visibility = ["//visibility:private"])
```

(unchanged; `tests/support/BUILD.bazel` sets its own public visibility)

- [ ] **Step 8: Run the tests to verify they pass**

Run: `bazel test //tests/unit/inference:chat_backend_test`
Expected: PASS — 3 tests.

- [ ] **Step 9: Verify the alias broke nothing**

Run: `bazel test //tests/unit/workflow:sub_agent_runtime_test //tests/unit/nodes:llm_node_test`
Expected: PASS, both — **unmodified**. This proves the `SendFn` contract survived, which is this plan's central premise. If either fails to compile, the alias in Step 5 is wrong; fix it there rather than editing the tests.

- [ ] **Step 10: Commit**

```bash
git add agentflow/inference/chat_backend.h agentflow/inference/BUILD.bazel \
        agentflow/inference/litert_lm_conversation.h \
        tests/support/ tests/unit/inference/
git commit -m "feat(inference): add IChatBackend/IConversation seam

Promotes SubAgentRuntime's SendFn contract into a public inference-layer
interface. LiteRtLmConversationOptions becomes an alias of the new
ChatConversationOptions so every existing call site compiles unchanged.

The seam gets its own Bazel target with no LiteRT-LM dependency, so
consumers of the interface do not drag the engine in."
```

---

## Task 3: Canonical message helpers (pure, no I/O)

Spec §3.1, §3.4. Extracts the stream-envelope logic currently buried in `AgentNode` into pure, directly testable functions. Nothing is wired up yet.

**Files:**
- Create: `agentflow/inference/canonical_message.h`
- Create: `agentflow/inference/canonical_message.cc`
- Create: `tests/unit/inference/canonical_message_test.cc`
- Modify: `agentflow/inference/BUILD.bazel`
- Modify: `tests/unit/inference/BUILD.bazel`

**Interfaces:**
- Consumes: nothing from earlier tasks (pure JSON helpers).
- Produces:
  - `agentflow::ExtractAssistantText(std::string_view canonical_json) -> std::string`
  - `agentflow::LiteRtStreamAssembler` with `void Feed(std::string_view chunk)`, `const std::vector<std::string>& text_deltas() const`, `std::string Canonical() const`
  - Task 4 (`LiteRtLmChatBackend`) and Task 5 (`AgentNode`) both consume these.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/inference/canonical_message_test.cc`:

```cpp
// tests/unit/inference/canonical_message_test.cc
#include "agentflow/inference/canonical_message.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace agentflow {
namespace {

using json = nlohmann::json;

TEST(ExtractAssistantTextTest, ConcatenatesAllTextItems) {
  EXPECT_EQ(ExtractAssistantText(
                R"({"role":"assistant","content":[{"type":"text","text":"ab"},)"
                R"({"type":"text","text":"cd"}]})"),
            "abcd");
}

TEST(ExtractAssistantTextTest, IgnoresNonTextItemsAndMissingContent) {
  EXPECT_EQ(ExtractAssistantText(
                R"({"role":"assistant","content":[{"type":"image"},)"
                R"({"type":"text","text":"ok"}]})"),
            "ok");
  EXPECT_EQ(ExtractAssistantText(R"({"role":"assistant"})"), "");
  EXPECT_EQ(ExtractAssistantText("not json at all"), "");
}

TEST(LiteRtStreamAssemblerTest, AccumulatesTextEnvelopesIntoCanonical) {
  LiteRtStreamAssembler a;
  a.Feed(R"({"role":"assistant","content":[{"type":"text","text":"He"}]})");
  a.Feed(R"({"role":"assistant","content":[{"type":"text","text":"llo"}]})");

  EXPECT_EQ(a.text_deltas(), (std::vector<std::string>{"He", "llo"}));
  EXPECT_EQ(json::parse(a.Canonical()),
            json::parse(
                R"({"role":"assistant","content":[{"type":"text","text":"Hello"}]})"));
}

TEST(LiteRtStreamAssemblerTest, ToolCallEnvelopeWinsOverAccumulatedText) {
  // LiteRT-LM emits a complete tool_calls message as its own chunk. When one
  // arrives it IS the turn's response; earlier text is not the final answer.
  LiteRtStreamAssembler a;
  a.Feed(R"({"role":"assistant","content":[{"type":"text","text":"thinking"}]})");
  a.Feed(R"({"role":"assistant","tool_calls":[{"id":"call_1",)"
         R"("function":{"name":"search","arguments":"{\"q\":\"x\"}"}}]})");

  json got = json::parse(a.Canonical());
  ASSERT_TRUE(got.contains("tool_calls"));
  EXPECT_EQ(got["tool_calls"][0]["id"], "call_1");
  EXPECT_EQ(got["tool_calls"][0]["function"]["name"], "search");
}

TEST(LiteRtStreamAssemblerTest, NonJsonChunkIsTreatedAsRawTextDelta) {
  LiteRtStreamAssembler a;
  a.Feed("plain");
  a.Feed(" text");

  EXPECT_EQ(a.text_deltas(), (std::vector<std::string>{"plain", " text"}));
  EXPECT_EQ(ExtractAssistantText(a.Canonical()), "plain text");
}

TEST(LiteRtStreamAssemblerTest, EmptyStreamYieldsEmptyAssistantMessage) {
  LiteRtStreamAssembler a;
  json got = json::parse(a.Canonical());
  EXPECT_EQ(got["role"], "assistant");
  EXPECT_EQ(ExtractAssistantText(a.Canonical()), "");
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test //tests/unit/inference:canonical_message_test`
Expected: FAIL — `agentflow/inference/canonical_message.h` does not exist.

- [ ] **Step 3: Write the header**

Create `agentflow/inference/canonical_message.h`:

```cpp
// agentflow/inference/canonical_message.h
//
// Pure helpers over the canonical assistant-message shape. No engine, no
// network, no io_context — every function here is directly unit-testable.
//
// The canonical shape is LiteRT-LM's existing response shape:
//   {"role":"assistant",
//    "content":[{"type":"text","text":"..."}],
//    "tool_calls":[{"id":"...","function":{"name":"...","arguments":"{...}"}}]}
#ifndef AGENTFLOW_INFERENCE_CANONICAL_MESSAGE_H_
#define AGENTFLOW_INFERENCE_CANONICAL_MESSAGE_H_

#include <string>
#include <string_view>
#include <vector>

namespace agentflow {

// Concatenates every {"type":"text"} item in `canonical_json`'s content array.
// Returns "" if the input is not parseable, has no content array, or holds no
// text items.
std::string ExtractAssistantText(std::string_view canonical_json);

// Accumulates LiteRT-LM stream envelopes into one canonical assistant message.
//
// LiteRT-LM streams each delta as a FULL message envelope wrapping one
// incremental piece, e.g.
//   {"role":"assistant","content":[{"type":"text","text":"The"}]}
// for text, or a complete
//   {"role":"assistant","tool_calls":[...]}
// for a tool call. Raw concatenation of envelopes is not valid JSON, so this
// class rebuilds a single canonical message instead.
class LiteRtStreamAssembler {
 public:
  // Feeds one stream chunk. A chunk that does not parse as JSON is treated as
  // a plain text delta (the engine occasionally emits raw text).
  void Feed(std::string_view chunk);

  // Text deltas seen so far, in arrival order. The caller forwards these to a
  // TokenSink / EventEmitter.
  const std::vector<std::string>& text_deltas() const { return deltas_; }

  // The canonical assistant JSON for everything fed so far. If any chunk
  // carried tool_calls, that message is returned (a tool call IS the turn's
  // response); otherwise a text message built from the accumulated deltas.
  std::string Canonical() const;

 private:
  std::vector<std::string> deltas_;
  std::string text_;
  std::string tool_call_json_;  // empty when no tool call was seen
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_CANONICAL_MESSAGE_H_
```

- [ ] **Step 4: Write the implementation**

Create `agentflow/inference/canonical_message.cc`:

```cpp
// agentflow/inference/canonical_message.cc
#include "agentflow/inference/canonical_message.h"

#include <nlohmann/json.hpp>

namespace agentflow {
namespace {
using json = nlohmann::json;
}  // namespace

std::string ExtractAssistantText(std::string_view canonical_json) {
  json resp = json::parse(canonical_json, nullptr, /*allow_exceptions=*/false);
  if (resp.is_discarded() || !resp.contains("content")) return {};
  const auto& content = resp["content"];
  if (!content.is_array()) return {};
  std::string out;
  for (const auto& item : content) {
    if (item.value("type", "") == "text" && item.contains("text") &&
        item["text"].is_string()) {
      out.append(item["text"].get<std::string>());
    }
  }
  return out;
}

void LiteRtStreamAssembler::Feed(std::string_view chunk) {
  if (chunk.empty()) return;

  json cj = json::parse(chunk, nullptr, /*allow_exceptions=*/false);
  if (cj.is_discarded()) {
    // Raw, non-JSON chunk — the engine emitted plain text.
    deltas_.emplace_back(chunk);
    text_.append(chunk);
    return;
  }

  if (cj.contains("tool_calls") && cj["tool_calls"].is_array() &&
      !cj["tool_calls"].empty()) {
    tool_call_json_ = cj.dump();
    return;
  }

  std::string delta = ExtractAssistantText(chunk);
  if (!delta.empty()) {
    deltas_.push_back(delta);
    text_.append(delta);
  }
}

std::string LiteRtStreamAssembler::Canonical() const {
  if (!tool_call_json_.empty()) return tool_call_json_;
  json msg = {
      {"role", "assistant"},
      {"content", json::array({{{"type", "text"}, {"text", text_}}})},
  };
  return msg.dump();
}

}  // namespace agentflow
```

- [ ] **Step 5: Add the Bazel targets**

In `agentflow/inference/BUILD.bazel`, add a new library above `inference`:

```python
cc_library(
    name = "canonical_message",
    srcs = ["canonical_message.cc"],
    hdrs = ["canonical_message.h"],
    deps = ["@nlohmann_json//:json"],
)
```

Append to `tests/unit/inference/BUILD.bazel`:

```python
cc_test(
    name = "canonical_message_test",
    size = "small",
    srcs = ["canonical_message_test.cc"],
    deps = [
        "//agentflow/inference:canonical_message",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test //tests/unit/inference:canonical_message_test`
Expected: PASS — 6 tests.

- [ ] **Step 7: Commit**

```bash
git add agentflow/inference/canonical_message.h \
        agentflow/inference/canonical_message.cc \
        agentflow/inference/BUILD.bazel tests/unit/inference/
git commit -m "feat(inference): pure canonical-message helpers

Extracts ExtractAssistantText and LiteRT stream-envelope assembly into
directly testable pure functions, ahead of moving them out of AgentNode."
```

---

## Task 4: `LiteRtLmChatBackend` — today's engine behind the seam

Spec §3.3. The logic already exists twice: in `AgentNode::Run`
(`agent_node.cc:117-197`) and in `SubAgentRuntime::DefaultConversationFactory`
(`sub_agent_runtime.cc:69-130`). This task writes it once, in the place it
belongs. Nothing is rewired yet — Tasks 5 and 6 do that.

**Files:**
- Create: `agentflow/inference/litert_lm_chat_backend.h`
- Create: `agentflow/inference/litert_lm_chat_backend.cc`
- Create: `tests/unit/inference/litert_lm_chat_backend_test.cc`
- Modify: `agentflow/inference/BUILD.bazel`
- Modify: `tests/unit/inference/BUILD.bazel`

**Interfaces:**
- Consumes: `IChatBackend`, `IConversation`, `ChatConversationOptions`, `TokenSink` (Task 2); `LiteRtStreamAssembler`, `ExtractAssistantText` (Task 3); existing `LiteRtLmEngine`, `LiteRtLmConversation`.
- Produces: `agentflow::LiteRtLmChatBackend::Create(std::shared_ptr<LiteRtLmEngine>, asio::io_context&) -> std::shared_ptr<IChatBackend>`. Tasks 6 and 12 construct backends through it.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/inference/litert_lm_chat_backend_test.cc`:

```cpp
// tests/unit/inference/litert_lm_chat_backend_test.cc
//
// End-to-end decode needs a real engine, so that case requires MODEL_PATH and
// skips itself. What IS asserted without a model is the seam contract:
// construction, Describe(), and the documented null-engine failure mode.
#include "agentflow/inference/litert_lm_chat_backend.h"

#include <cstdlib>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/inference/canonical_message.h"
#include "agentflow/inference/litert_lm_engine.h"

namespace agentflow {
namespace {

TEST(LiteRtLmChatBackendTest, DescribeIsStableAndCredentialFree) {
  asio::io_context io;
  auto backend = LiteRtLmChatBackend::Create(nullptr, io);
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->Describe(), "litert-lm");
}

TEST(LiteRtLmChatBackendTest, NullEngineYieldsNullConversation) {
  // A null engine cannot build a conversation. CreateConversation returns
  // nullptr rather than throwing; callers treat that as engine_error.
  asio::io_context io;
  auto backend = LiteRtLmChatBackend::Create(nullptr, io);
  EXPECT_EQ(backend->CreateConversation(ChatConversationOptions{}), nullptr);
}

TEST(LiteRtLmChatBackendTest, SendAsyncReturnsCanonicalAssistantJson) {
  const char* model_path = std::getenv("MODEL_PATH");
  if (!model_path) GTEST_SKIP() << "MODEL_PATH not set";

  asio::io_context io;
  auto engine = LiteRtLmEngine::Create(
      LiteRtLmEngineOptions{.model_path = model_path});
  ASSERT_NE(engine, nullptr);

  auto backend = LiteRtLmChatBackend::Create(engine, io);
  ChatConversationOptions opts;
  opts.system_message_json = R"([{"type":"text","text":"Answer briefly."}])";
  auto conv = backend->CreateConversation(std::move(opts));
  ASSERT_NE(conv, nullptr);

  std::string got;
  std::vector<std::string> deltas;
  CancelSource cancel_src;
  const CancelToken cancel = cancel_src.Token();
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    auto r = co_await conv->SendAsync(
        R"({"role":"user","content":[{"type":"text","text":"Say hi."}]})",
        [&](std::string_view d) { deltas.emplace_back(d); }, cancel);
    ASSERT_TRUE(r.ok()) << r.status().message();
    got = *r;
  }, asio::detached);
  io.run();

  EXPECT_NE(got.find("\"role\":\"assistant\""), std::string::npos);
  std::string joined;
  for (const auto& d : deltas) joined += d;
  EXPECT_EQ(joined, ExtractAssistantText(got));
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test //tests/unit/inference:litert_lm_chat_backend_test`
Expected: FAIL — `agentflow/inference/litert_lm_chat_backend.h` does not exist.

- [ ] **Step 3: Write the header**

Create `agentflow/inference/litert_lm_chat_backend.h`:

```cpp
// agentflow/inference/litert_lm_chat_backend.h
#ifndef AGENTFLOW_INFERENCE_LITERT_LM_CHAT_BACKEND_H_
#define AGENTFLOW_INFERENCE_LITERT_LM_CHAT_BACKEND_H_

#include <memory>
#include <string_view>
#include <utility>

#include <asio/io_context.hpp>

#include "agentflow/inference/chat_backend.h"
#include "agentflow/inference/litert_lm_engine.h"

namespace agentflow {

// IChatBackend over the on-device LiteRT-LM engine.
//
// Owns the stream-envelope handling that used to be duplicated in
// AgentNode::Run and SubAgentRuntime::DefaultConversationFactory. History
// lives in the engine, so successive SendAsync calls on one conversation form
// a multi-turn exchange and the KV cache is reused across turns.
class LiteRtLmChatBackend : public IChatBackend {
 public:
  // `engine` may be null; CreateConversation then returns nullptr.
  // `io` must outlive the backend and every conversation it creates.
  static std::shared_ptr<IChatBackend> Create(
      std::shared_ptr<LiteRtLmEngine> engine, asio::io_context& io);

  std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) override;

  std::string_view Describe() const override { return "litert-lm"; }

 private:
  LiteRtLmChatBackend(std::shared_ptr<LiteRtLmEngine> engine,
                       asio::io_context& io)
      : engine_(std::move(engine)), io_(io) {}

  std::shared_ptr<LiteRtLmEngine> engine_;
  asio::io_context& io_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_LITERT_LM_CHAT_BACKEND_H_
```

- [ ] **Step 4: Write the implementation**

Create `agentflow/inference/litert_lm_chat_backend.cc`:

```cpp
// agentflow/inference/litert_lm_chat_backend.cc
#include "agentflow/inference/litert_lm_chat_backend.h"

#include <atomic>
#include <exception>
#include <string>

#include <absl/status/status.h>

#include "agentflow/inference/canonical_message.h"
#include "agentflow/inference/litert_lm_conversation.h"

namespace agentflow {
namespace {

class LiteRtLmChatConversation : public IConversation {
 public:
  LiteRtLmChatConversation(std::shared_ptr<LiteRtLmConversation> conv,
                            bool constrained)
      : conv_(std::move(conv)), constrained_(constrained) {}

  asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) override {
    // Register the in-flight cancel hook once: a cancel must break the engine
    // request mid-decode, not merely at the next turn boundary.
    if (!cancel_registered_.exchange(true)) {
      auto conv = conv_;
      cancel.OnCancel([conv]() { conv->Cancel(); });
    }

    // Non-streaming path. The constrained C bridge has no streaming variant
    // (litert_lm_conversation_send_message_stream ignores the grammar), and a
    // missing sink means nobody wants deltas.
    if (constrained_ || !on_token) {
      co_return conv_->SendMessageSync(message_json);
    }

    conv_->SendMessage(std::move(message_json));
    LiteRtStreamAssembler assembler;
    for (;;) {
      std::string chunk;
      try {
        chunk = co_await conv_->NextTokenAsync();
      } catch (const std::exception&) {
        co_return absl::InternalError(
            "litert-lm: streaming send failed mid-decode");
      }
      if (chunk.empty()) break;  // end of turn

      const size_t before = assembler.text_deltas().size();
      assembler.Feed(chunk);
      // Forward only newly produced text deltas, never the raw envelope.
      for (size_t i = before; i < assembler.text_deltas().size(); ++i) {
        on_token(assembler.text_deltas()[i]);
      }
    }
    co_return assembler.Canonical();
  }

  void Cancel() override { conv_->Cancel(); }

 private:
  std::shared_ptr<LiteRtLmConversation> conv_;
  bool constrained_;
  std::atomic<bool> cancel_registered_{false};
};

}  // namespace

std::shared_ptr<IChatBackend> LiteRtLmChatBackend::Create(
    std::shared_ptr<LiteRtLmEngine> engine, asio::io_context& io) {
  return std::shared_ptr<IChatBackend>(
      new LiteRtLmChatBackend(std::move(engine), io));
}

std::shared_ptr<IConversation> LiteRtLmChatBackend::CreateConversation(
    ChatConversationOptions opts) {
  if (!engine_) return nullptr;
  const bool constrained = opts.constrained_tool_calls;
  auto conv = LiteRtLmConversation::Create(engine_, std::move(opts), io_);
  if (!conv) return nullptr;
  return std::make_shared<LiteRtLmChatConversation>(std::move(conv),
                                                     constrained);
}

}  // namespace agentflow
```

- [ ] **Step 5: Add the Bazel targets**

In `agentflow/inference/BUILD.bazel`, add `"litert_lm_chat_backend.cc"` to the
`inference` target's `srcs`, `"litert_lm_chat_backend.h"` to its `hdrs`, and
`":canonical_message"` to its `deps`.

Append to `tests/unit/inference/BUILD.bazel`:

```python
cc_test(
    name = "litert_lm_chat_backend_test",
    size = "small",
    srcs = ["litert_lm_chat_backend_test.cc"],
    deps = [
        "//agentflow/core",
        "//agentflow/inference",
        "//agentflow/inference:canonical_message",
        "//agentflow/inference:chat_backend",
        "@asio",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

Note this target is **not** tagged `manual` — the two no-model cases must run
in CI; the third skips itself.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test //tests/unit/inference:litert_lm_chat_backend_test --test_output=all`
Expected: PASS — 2 run, 1 skipped (`MODEL_PATH not set`).

- [ ] **Step 7: Commit**

```bash
git add agentflow/inference/litert_lm_chat_backend.h \
        agentflow/inference/litert_lm_chat_backend.cc \
        agentflow/inference/BUILD.bazel tests/unit/inference/
git commit -m "feat(inference): LiteRtLmChatBackend implementing IChatBackend

Single home for the stream-envelope handling that was duplicated between
AgentNode::Run and SubAgentRuntime::DefaultConversationFactory. Both are
rewired onto it in the following tasks."
```

---

## Task 5: Migrate `AgentNode` onto the seam, with `id` passthrough

Spec §3.2, §3.4, §7.4. The largest task: swap the dependency, collapse the two
run branches, add tool-call `id` passthrough, and — because a fake backend is
now injectable — un-skip the `AgentNode` tests that have never run in CI.

**Files:**
- Modify: `agentflow/nodes/agent_node.h` (`engine` → `backend`)
- Modify: `agentflow/nodes/agent_node.cc:85-241`
- Modify: `agentflow/nodes/BUILD.bazel`
- Modify: `tests/unit/nodes/agent_node_test.cc`
- Modify: `tests/unit/nodes/BUILD.bazel`

**Interfaces:**
- Consumes: `IChatBackend`, `ChatConversationOptions`, `TokenSink`, `FakeChatBackend` (Task 2); `ExtractAssistantText` (Task 3).
- Produces: `AgentNodeConfig.backend` of type `std::shared_ptr<IChatBackend>` replacing `.engine`. Tasks 6 and 12 set it.

**Real symbol names in this codebase** (verified — use exactly these):
`test::TestState` with fields `user_query` / `assistant_reply`;
`State::From(std::move(raw))` and `out.As<test::TestState>()`;
`CancelSource cancel; cancel.Token()`;
`CallbackEventEmitter` from `agentflow/observability/callback_event_emitter.h`
taking `[](const proto::TraceEvent&)`; token deltas arrive as
`e.has_token()` / `e.token().token()`; `NativeFnTool(ToolSchema, Fn)` where
`Fn = std::function<asio::awaitable<std::string>(std::string_view, const CancelToken&)>`;
`ToolRegistry::Register(std::shared_ptr<Tool>)`.

- [ ] **Step 1: Write the failing tests — the ReAct loop with no model**

Replace `tests/unit/nodes/agent_node_test.cc` entirely:

```cpp
// tests/unit/nodes/agent_node_test.cc
//
// These cases used to require MODEL_PATH and were skipped in CI. AgentNode now
// takes an IChatBackend, so a scripted fake exercises the ReAct loop, tool
// dispatch and the iteration limit with no model file.

#include "agentflow/nodes/agent_node.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/chat_backend.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool_registry.h"
#include "test_messages.pb.h"
#include "tests/support/fake_chat_backend.h"

namespace agentflow {
namespace {

using json = nlohmann::json;

struct EventCapture {
  std::vector<proto::TraceEvent> events;
  std::mutex m;
  CallbackEventEmitter emitter{[this](const proto::TraceEvent& e) {
    std::lock_guard<std::mutex> l(m);
    events.push_back(e);
  }};

  std::vector<std::string> tokens() {
    std::lock_guard<std::mutex> l(m);
    std::vector<std::string> out;
    for (const auto& e : events) {
      if (e.has_token()) out.push_back(e.token().token());
    }
    return out;
  }
};

AgentNodeConfig BaseConfig(std::shared_ptr<IChatBackend> backend,
                            asio::io_context& io) {
  AgentNodeConfig cfg;
  cfg.backend = std::move(backend);
  cfg.io_ctx = &io;
  cfg.system_prompt = "You are a test agent.";
  cfg.input_field = "user_query";
  cfg.output_field = "assistant_reply";
  cfg.stream_tokens = true;
  return cfg;
}

// Runs the node to completion on `io` and returns the resulting State.
State RunNode(AgentNodeConfig cfg, const std::string& query,
               asio::io_context& io, EventCapture& cap) {
  auto node = std::make_unique<AgentNode>(std::move(cfg));
  test::TestState raw;
  raw.set_user_query(query);

  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        State state = State::From(std::move(raw));
        co_return co_await node->Run(std::move(state), cancel.Token(),
                                      cap.emitter);
      },
      asio::use_future);
  io.run();
  return fut.get();
}

std::shared_ptr<ToolRegistry> RegistryWith(
    const std::string& name, std::string canned_result) {
  auto registry = std::make_shared<ToolRegistry>();
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = name,
                 .description = "test tool",
                 .params_json_schema = R"({"type":"object","properties":{}})"},
      [canned = std::move(canned_result)](std::string_view,
                                           const CancelToken&)
          -> asio::awaitable<std::string> { co_return canned; }));
  return registry;
}

TEST(AgentNodeTest, PlainAnswerIsWrittenToOutputField) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","content":[{"type":"text","text":"42"}]})"});

  EventCapture cap;
  State out = RunNode(BaseConfig(backend, io), "what is 6*7?", io, cap);
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "42");
}

TEST(AgentNodeTest, SystemPromptAndToolsReachTheBackend) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","content":[{"type":"text","text":"ok"}]})"});

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = RegistryWith("search", "result");
  EventCapture cap;
  RunNode(std::move(cfg), "hi", io, cap);

  // The system message is a BARE content array, not a {role,content} object.
  EXPECT_EQ(json::parse(backend->last_options().system_message_json),
            json::parse(R"([{"type":"text","text":"You are a test agent."}])"));

  // BuildToolsJson already emits the OpenAI tools shape.
  json tools = json::parse(backend->last_options().tools_json);
  ASSERT_TRUE(tools.is_array());
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0]["type"], "function");
  EXPECT_EQ(tools[0]["function"]["name"], "search");
}

TEST(AgentNodeTest, ToolCallIsDispatchedAndItsIdEchoedBack) {
  asio::io_context io;
  // Turn 1: the model asks for a tool. Turn 2: it answers.
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[{"id":"call_7",)"
          R"("function":{"name":"search","arguments":"{\"q\":\"zen\"}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"found it"}]})"});

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = RegistryWith("search", "SEARCH_RESULT");
  EventCapture cap;
  State out = RunNode(std::move(cfg), "find zen", io, cap);

  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "found it");

  // The tool-result message must carry the originating call's id, so a remote
  // backend can restore OpenAI's tool_call_id (design spec §3.2).
  auto conv = backend->last_conversation();
  ASSERT_NE(conv, nullptr);
  ASSERT_EQ(conv->sent().size(), 2u);
  json tool_msg = json::parse(conv->sent()[1]);
  EXPECT_EQ(tool_msg["role"], "tool");
  ASSERT_EQ(tool_msg["content"].size(), 1u);
  EXPECT_EQ(tool_msg["content"][0]["id"], "call_7");
  EXPECT_EQ(tool_msg["content"][0]["name"], "search");
  EXPECT_EQ(tool_msg["content"][0]["response"]["value"], "SEARCH_RESULT");
}

TEST(AgentNodeTest, MaxIterReachedWritesTheFallbackMessage) {
  asio::io_context io;
  // Always asks for a tool, never answers.
  std::vector<std::string> loop(
      4,
      R"({"role":"assistant","tool_calls":[{"id":"c",)"
      R"("function":{"name":"noop","arguments":"{}"}}]})");
  auto backend = std::make_shared<testing::FakeChatBackend>(loop);

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = RegistryWith("noop", "");
  cfg.max_iter = 3;
  EventCapture cap;
  State out = RunNode(std::move(cfg), "spin", io, cap);

  EXPECT_EQ(out.As<test::TestState>().assistant_reply(),
            "Agent reached maximum iterations without a final answer.");
}

TEST(AgentNodeTest, StreamedDeltasReachTheEventEmitter) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","content":[{"type":"text","text":"ab"}]})"});
  backend->set_deltas({"a", "b"});

  EventCapture cap;
  RunNode(BaseConfig(backend, io), "hi", io, cap);
  EXPECT_EQ(cap.tokens(), (std::vector<std::string>{"a", "b"}));
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test //tests/unit/nodes:agent_node_test`
Expected: FAIL to compile — `AgentNodeConfig` has no member `backend`.

- [ ] **Step 3: Swap the dependency in the header**

In `agentflow/nodes/agent_node.h`:

- Replace the two includes
  `#include "agentflow/inference/litert_lm_conversation.h"` and
  `#include "agentflow/inference/litert_lm_engine.h"`
  with `#include "agentflow/inference/chat_backend.h"`.
- Replace the field `std::shared_ptr<LiteRtLmEngine> engine;` with:

```cpp
  // The inference backend. LiteRtLmChatBackend for on-device, or a remote
  // backend such as OpenAiChatBackend — AgentNode cannot tell them apart.
  std::shared_ptr<IChatBackend> backend;
```

Leave `BuildSystemMessageJson`, `BuildToolsJson`, `BuildUserMessageJson` and
`DispatchTool` declarations as they are.

- [ ] **Step 4: Collapse the two run branches**

In `agentflow/nodes/agent_node.cc`:

Delete the file-local `ExtractAssistantText` helper (lines 17-31) and add
`#include "agentflow/inference/canonical_message.h"` — Task 3 owns it now.
Add `#include <absl/status/status.h>` for `absl::IsCancelled`.

Replace the guard and conversation setup (lines 85-108) with:

```cpp
  if (!cfg_.backend || !cfg_.io_ctx) {
    throw AgentflowError("AgentNode: backend and io_ctx must be configured");
  }

  // One conversation per Run. The backend owns history across turns.
  ChatConversationOptions opts;
  opts.system_message_json = BuildSystemMessageJson();
  opts.tools_json = BuildToolsJson();
  opts.max_output_tokens = cfg_.max_output_tokens;
  opts.constrained_tool_calls = cfg_.constrained_tool_calls;

  auto conv = cfg_.backend->CreateConversation(std::move(opts));
  if (!conv) {
    throw AgentflowError(
        "AgentNode: failed to create conversation on backend " +
        std::string(cfg_.backend->Describe()));
  }

  // Cooperative cancellation: break the in-flight request so a streaming turn
  // stops mid-decode, not only at the next turn boundary. Safe from any
  // thread.
  cancel.OnCancel([conv]() { conv->Cancel(); });
```

Replace the entire
`if (cfg_.stream_tokens && !cfg_.constrained_tool_calls) { ... } else { ... }`
block (lines 117-197) with:

```cpp
    // One path for both streaming and non-streaming: the backend decides
    // whether deltas are available and reports them through the sink.
    TokenSink sink;
    if (cfg_.stream_tokens) {
      sink = [this, &emit](std::string_view delta) {
        emit.EmitToken(Id(), delta);
        if (cfg_.token_channel) {
          // Non-blocking. The sink is a plain callback invoked from inside the
          // backend, not a coroutine, so it cannot co_await a full channel.
          // try_send drops the delta if the consumer is not keeping up, which
          // is the right trade-off for a UI stream; a closed channel (consumer
          // gone) is a no-op rather than a throw.
          cfg_.token_channel->try_send(asio::error_code{}, std::string(delta));
        }
      };
    }

    auto resp_or = co_await conv->SendAsync(message_json, sink, cancel);
    if (!resp_or.ok()) {
      if (absl::IsCancelled(resp_or.status())) break;
      throw AgentflowError("AgentNode: backend send failed: " +
                            std::string(resp_or.status().message()));
    }
    std::string resp_str = *resp_or;
```

Then change the two calls to the deleted helper — `ExtractAssistantText(resp)`
— to `ExtractAssistantText(resp_str)`. The Task 3 version takes the JSON
**text**, not a parsed `json` object.

- [ ] **Step 5: Add `id` passthrough in the dispatch loop**

Replace the body of the tool-dispatch `for` loop (previously lines 216-241)
with:

```cpp
      json tool_content = json::array();
      for (const auto& tc : resp["tool_calls"]) {
        std::string name = tc.value("name", tc.value("function",
                                                      json::object())
                                                .value("name", ""));
        // The originating call's id. LiteRT-LM does not need it (the Gemma
        // template reads only name/response), but OpenAI-compatible backends
        // must echo it back as tool_call_id, so it is threaded through the
        // canonical shape. Design spec §3.2.
        std::string call_id = tc.value("id", "");
        std::string args;
        if (tc.contains("arguments")) {
          args = tc["arguments"].is_string()
                     ? tc["arguments"].get<std::string>()
                     : tc["arguments"].dump();
        } else if (tc.contains("function") &&
                   tc["function"].contains("arguments")) {
          args = tc["function"]["arguments"].is_string()
                     ? tc["function"]["arguments"].get<std::string>()
                     : tc["function"]["arguments"].dump();
        }
        std::string result = co_await DispatchTool(name, args, cancel, emit);
        // Per the Gemma jinja template each content item carries `name` +
        // `response` directly; `id` is additive and ignored on that path.
        json entry = {
          {"name", name},
          {"response", {{"value", result}}},
        };
        if (!call_id.empty()) entry["id"] = call_id;
        tool_content.push_back(std::move(entry));
      }
```

- [ ] **Step 6: Update the Bazel deps**

`--features=layering_check` rejects an include whose target is not in `deps`.

In `agentflow/nodes/BUILD.bazel`, in the `nodes` target replace
`"//agentflow/inference"` with:

```python
        "//agentflow/inference:canonical_message",
        "//agentflow/inference:chat_backend",
```

and add `"@abseil-cpp//absl/status"`.

In `tests/unit/nodes/BUILD.bazel`, for `agent_node_test`: **delete
`tags = ["manual"]`**, change `size = "large"` to `size = "small"`, and add:

```python
        "//agentflow/inference:chat_backend",
        "//tests/support:fake_chat_backend",
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `bazel test //tests/unit/nodes:agent_node_test --test_output=all`
Expected: PASS — 5 tests, **none skipped**. First CI coverage of `AgentNode`'s
ReAct loop.

- [ ] **Step 8: Verify nothing else regressed**

Run: `bazel test //tests/unit/... //tests/integration/...`
Expected: all PASS. `sub_agent_runtime_test` still compiles because Task 2 made
`LiteRtLmConversationOptions` an alias and Task 6 has not touched it yet.

- [ ] **Step 9: Commit**

```bash
git add agentflow/nodes/agent_node.h agentflow/nodes/agent_node.cc \
        agentflow/nodes/BUILD.bazel tests/unit/nodes/
git commit -m "refactor(nodes): AgentNode runs on IChatBackend

Collapses the streaming and non-streaming branches into one SendAsync call,
moving ~90 lines of LiteRT stream-envelope parsing into the backend. Threads
the tool-call id through the canonical tool-result message so OpenAI-compatible
backends can restore tool_call_id.

Un-skips the AgentNode tests: they drive a scripted fake backend instead of
requiring MODEL_PATH, so the ReAct loop, tool dispatch and iteration limit are
covered in CI for the first time."
```

---

## Task 6: Migrate `LlmNode`, `SubAgentRuntime` and `AgentNodeBuildSpec`

Spec §5.1. Finishes the migration so nothing outside `agentflow/inference/`
mentions `LiteRtLmEngine`. After this task the LiteRT dependency is fully
behind the seam.

**Files:**
- Modify: `agentflow/nodes/llm_node.h`, `agentflow/nodes/llm_node.cc`
- Modify: `agentflow/workflow/sub_agent_runtime.h`, `agentflow/workflow/sub_agent_runtime.cc:69-130`
- Modify: `agentflow/workflow/workflow_runner.h:24`, `agentflow/workflow/workflow_runner.cc:18,46,53`
- Modify: `agentflow/workflow/BUILD.bazel`
- Modify: `tests/unit/nodes/llm_node_test.cc`

**Interfaces:**
- Consumes: `IChatBackend`, `ChatConversationOptions` (Task 2); `LiteRtLmChatBackend::Create` (Task 4).
- Produces:
  - `LlmNodeConfig.backend` replacing `.engine`.
  - `SubAgentRuntime::DefaultConversationFactory(std::shared_ptr<IChatBackend>) -> ConversationFactory` (the `io_context&` parameter is **gone** — the backend already holds it).
  - `AgentNodeBuildSpec.backend` (default) and `AgentNodeBuildSpec.backends` (`std::map<std::string, std::shared_ptr<IChatBackend>>`), replacing `.engine` at `workflow_runner.h:24`. Task 7 validates against the map.

> **Intentional behaviour change in `LlmNode`.** It currently drives the raw
> **Session** API (`litert_lm_engine_create_session`) with a hand-built
> `{"messages":[…],"tools":[…]}` blob and concatenates raw tokens. After this
> task it drives a **Conversation** through the backend, so the engine applies
> the model's chat template and the output field receives the extracted
> assistant text. The text for a given prompt may differ slightly because
> templating now happens. This is the point — it is what lets `LlmNode` run
> against a cloud backend at all. Note it in the commit message.

- [ ] **Step 1: Update the `LlmNode` tests first**

In `tests/unit/nodes/llm_node_test.cc`, rename the middle case and swap the
field. Replace `TEST(LlmNodeTest, CtorRejectsMissingEngine)` with:

```cpp
TEST(LlmNodeTest, CtorRejectsMissingBackend) {
  LlmNodeConfig cfg;
  cfg.id = "llm";
  asio::io_context io;
  cfg.io_ctx = &io;
  // cfg.backend deliberately left null.
  EXPECT_THROW(LlmNode n(std::move(cfg)), AgentflowError);
}
```

In the other two cases, replace every `cfg.engine = <something>;` with
`cfg.backend = std::make_shared<testing::FakeChatBackend>(std::vector<std::string>{});`
and add `#include "tests/support/fake_chat_backend.h"`.

Add a new case proving `LlmNode` now runs against any backend:

```cpp
TEST(LlmNodeTest, WritesAssistantTextToOutputField) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","content":[{"type":"text","text":"pong"}]})"});

  LlmNodeConfig cfg;
  cfg.id = "llm";
  cfg.io_ctx = &io;
  cfg.backend = backend;
  cfg.input_field = "user_query";
  cfg.output_field = "assistant_reply";
  LlmNode node(std::move(cfg));

  test::TestState raw;
  raw.set_user_query("ping");
  CancelSource cancel;
  EventCapture cap;  // same helper struct as in agent_node_test.cc

  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node.Run(State::From(std::move(raw)),
                                     cancel.Token(), cap.emitter);
      },
      asio::use_future);
  io.run();

  EXPECT_EQ(fut.get().As<test::TestState>().assistant_reply(), "pong");
}
```

Copy the `EventCapture` struct verbatim from the Task 5 test file into this
file's anonymous namespace — the two test binaries do not share code.

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test //tests/unit/nodes:llm_node_test`
Expected: FAIL to compile — `LlmNodeConfig` has no member `backend`.

- [ ] **Step 3: Migrate `LlmNode`**

In `agentflow/nodes/llm_node.h`: replace
`#include "agentflow/inference/litert_lm_engine.h"` and
`#include "c/engine.h"` with
`#include "agentflow/inference/chat_backend.h"`; replace the field
`std::shared_ptr<LiteRtLmEngine> engine;` with
`std::shared_ptr<IChatBackend> backend;`; delete the
`LiteRtLmSamplerParams sampler{};` field (it was only meaningful for the raw
Session API) and the `BuildConversationJson` declaration.

In `agentflow/nodes/llm_node.cc`: drop the
`#include "agentflow/inference/litert_lm_session.h"` and
`#include "c/engine.h"` includes, add
`#include "agentflow/inference/canonical_message.h"`, delete
`BuildConversationJson`, and replace the constructor check and `Run` with:

```cpp
LlmNode::LlmNode(LlmNodeConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.id.empty()) throw AgentflowError("LlmNode: id required");
  if (!cfg_.backend) throw AgentflowError("LlmNode: backend required");
  if (!cfg_.io_ctx) throw AgentflowError("LlmNode: io_ctx required");
}

asio::awaitable<State> LlmNode::Run(State state, const CancelToken& cancel,
                                     EventEmitter& emit) {
  if (cancel.IsCancelled()) co_return std::move(state);

  ChatConversationOptions opts;
  if (!cfg_.system_prompt.empty()) {
    // A BARE content array — the backend wraps it into {role:system,...}.
    json sys = json::array({{{"type", "text"}, {"text", cfg_.system_prompt}}});
    opts.system_message_json = sys.dump();
  }
  // Publish tool schemas so the model can emit function-calling JSON. LlmNode
  // never dispatches a call — the raw reply is left for the next node.
  if (cfg_.tool_registry) {
    opts.tools_json = cfg_.tool_registry->ExportToolsJson(cfg_.tool_names);
  }
  opts.max_output_tokens = cfg_.max_output_tokens;

  auto conv = cfg_.backend->CreateConversation(std::move(opts));
  if (!conv) {
    throw AgentflowError("LlmNode: failed to create conversation on backend " +
                          std::string(cfg_.backend->Describe()));
  }
  cancel.OnCancel([conv]() { conv->Cancel(); });

  TokenSink sink;
  if (cfg_.stream_tokens) {
    sink = [this, &emit](std::string_view delta) { emit.EmitToken(Id(), delta); };
  }

  json user = {
      {"role", "user"},
      {"content", json::array({{{"type", "text"},
                                {"text", ReadField(state, cfg_.input_field)}}})},
  };
  auto resp_or = co_await conv->SendAsync(user.dump(), sink, cancel);
  if (!resp_or.ok()) {
    if (absl::IsCancelled(resp_or.status())) co_return std::move(state);
    throw AgentflowError("LlmNode: backend send failed: " +
                          std::string(resp_or.status().message()));
  }

  // The raw reply may carry tool_calls; the next node interprets it. When it
  // is plain text, write the extracted text rather than the JSON envelope.
  std::string text = ExtractAssistantText(*resp_or);
  WriteOutput(state, text.empty() ? *resp_or : text);
  co_return std::move(state);
}
```

Add `#include <absl/status/status.h>` for `absl::IsCancelled`.

- [ ] **Step 4: Migrate `SubAgentRuntime`**

In `agentflow/workflow/sub_agent_runtime.h`:

- Replace the forward declaration `namespace agentflow { class LiteRtLmEngine; }`
  with `#include "agentflow/inference/chat_backend.h"`.
- Replace `using TokenSink = std::function<void(std::string_view delta)>;`
  with an alias so there is exactly one definition:
  ```cpp
  // One definition lives in agentflow/inference/chat_backend.h. The signature
  // is unchanged, so every existing caller compiles as before.
  using TokenSink = ::agentflow::TokenSink;
  ```
- Change `using ConversationFactory = std::function<SendFn(LiteRtLmConversationOptions)>;`
  to `std::function<SendFn(::agentflow::ChatConversationOptions)>` (identical
  type — `LiteRtLmConversationOptions` is already an alias — but state the real
  name now that LiteRT is out of this header).
- Change the factory declaration to:
  ```cpp
  // Builds the production factory from any chat backend — on-device or remote.
  static ConversationFactory DefaultConversationFactory(
      std::shared_ptr<::agentflow::IChatBackend> backend);
  ```

In `agentflow/workflow/sub_agent_runtime.cc`, replace the whole
`DefaultConversationFactory` body (lines 69-130) with:

```cpp
SubAgentRuntime::ConversationFactory
SubAgentRuntime::DefaultConversationFactory(
    std::shared_ptr<::agentflow::IChatBackend> backend) {
  return [backend = std::move(backend)](
             ::agentflow::ChatConversationOptions opts) -> SendFn {
    auto conv = backend->CreateConversation(std::move(opts));
    if (!conv) return SendFn{};
    return [conv](const std::string& message_json, const TokenSink& on_token,
                   const ::agentflow::CancelToken& cancel)
               -> asio::awaitable<absl::StatusOr<std::string>> {
      co_return co_await conv->SendAsync(message_json, on_token, cancel);
    };
  };
}
```

Delete the now-unused `#include "agentflow/inference/litert_lm_engine.h"` and
`#include "agentflow/inference/litert_lm_conversation.h"` from that file, and
add `#include "agentflow/inference/chat_backend.h"`.

> This is the ~60-line deletion the design predicted: the streaming loop, the
> `std::atomic_bool` cancel-registration guard and the envelope rebuild all now
> live in `LiteRtLmChatBackend` (Task 4).

- [ ] **Step 5: Migrate `AgentNodeBuildSpec`**

In `agentflow/workflow/workflow_runner.h`, replace
`std::shared_ptr<::agentflow::LiteRtLmEngine> engine;` with:

```cpp
  // Default inference backend, used by any agent whose ModelSpec.backend is
  // empty.
  std::shared_ptr<::agentflow::IChatBackend> backend;

  // Named backends the host registered. An agent selects one by logical name
  // via ModelSpec.backend. Credentials live in the host-constructed instance,
  // never in the spec.
  std::map<std::string, std::shared_ptr<::agentflow::IChatBackend>> backends;
```

Add `#include <map>` and `#include "agentflow/inference/chat_backend.h"`, and
remove the `namespace agentflow { class LiteRtLmEngine; }` forward declaration
at `workflow_runner.h:14`.

In `agentflow/workflow/workflow_runner.cc`:

- line 18: `cfg.engine = spec.engine;` → resolve by name:
  ```cpp
  cfg.backend = ResolveBackend(spec, agent_def);
  ```
- line 46: `if (agent_def.has_delegates() && spec.engine && spec.io_ctx)` →
  `if (agent_def.has_delegates() && cfg.backend && spec.io_ctx)`
- line 53: `SubAgentRuntime::DefaultConversationFactory(spec.engine, *spec.io_ctx)`
  → `SubAgentRuntime::DefaultConversationFactory(cfg.backend)`

**For this task, set `cfg.backend = spec.backend;` directly.** The
`ModelSpec.backend` proto field does not exist yet, so per-agent resolution
cannot be written until Task 7 adds it. The `backends` map is declared here but
stays unread until then.

- [ ] **Step 6: Update Bazel deps**

In `agentflow/workflow/BUILD.bazel`, replace `"//agentflow/inference"` in the
`workflow` target's `deps` with `"//agentflow/inference:chat_backend"`.
`layering_check` will flag anything still reaching for LiteRT headers.

In `tests/unit/nodes/BUILD.bazel`, add to `llm_node_test`'s deps:

```python
        "//agentflow/inference:chat_backend",
        "//agentflow/observability",
        "//proto:agentflow_proto",
        "//tests/support:fake_chat_backend",
        "@nlohmann_json//:json",
```

- [ ] **Step 7: Run the full suite**

Run: `bazel test //tests/unit/... //tests/integration/...`
Expected: all PASS, including `sub_agent_runtime_test` **unmodified**.

If `sub_agent_runtime_test` fails to compile, the `ConversationFactory`
signature drifted. Fix the alias in `sub_agent_runtime.h` — do **not** edit the
test. Its staying unchanged is the whole point.

- [ ] **Step 8: Verify LiteRT is fully behind the seam**

Run: `grep -rn "litert_lm_engine.h\|LiteRtLmEngine" agentflow/ --include=*.h --include=*.cc | grep -v "^agentflow/inference/"`
Expected: **no output**. Any hit outside `agentflow/inference/` means a call
site was missed.

- [ ] **Step 9: Commit**

```bash
git add agentflow/nodes/ agentflow/workflow/ tests/unit/nodes/
git commit -m "refactor: move LlmNode, SubAgentRuntime and WorkflowSpec onto IChatBackend

LiteRT-LM is now referenced only inside agentflow/inference/. WorkflowSpec
carries a default backend plus a host-populated map of named backends.

DefaultConversationFactory drops ~60 lines: the streaming loop, the
cancel-registration guard and the envelope rebuild all live in
LiteRtLmChatBackend now.

BEHAVIOUR CHANGE: LlmNode moves from the raw Session API to a Conversation, so
the engine applies the model's chat template and the output field receives
extracted assistant text. This is what lets LlmNode target a cloud backend."
```

---

## Task 7: `ModelSpec.backend` — per-agent backend selection

Spec §5. Adds the logical-name field and the resolution that turns it into a
concrete backend, with a hard error when the name is unknown.

> **Spec correction to apply in this task.** The design spec §5 says an unknown
> backend name is *"a load-time error, reported by `workflow_loader`"*. That is
> not implementable as written: `WorkflowLoader::Load` parses JSON into the
> proto and has no access to the host's backends map, which is a runtime
> object supplied later via `AgentNodeBuildSpec`. Validation therefore happens
> at **build time**, in `BuildAgentNode`, which is the first point where both
> the parsed `ModelSpec` and the map exist. `BuildAgentNode` returns by value
> with no status channel, so it throws `AgentflowError` — the same failure
> style `AgentNode`'s own constructor uses. The guarantee the spec cared about
> is preserved exactly: an unknown name never silently falls back to the
> default backend. Update spec §5 as part of this task's commit.

**Files:**
- Modify: `proto/workflow_spec.proto:28-31`
- Modify: `agentflow/workflow/workflow_loader.cc` (parse `model.backend`)
- Modify: `agentflow/workflow/workflow_runner.cc` (resolve + reject)
- Modify: `docs/superpowers/specs/2026-08-11-remote-llm-backend-design.md` (§5 correction)
- Create: `tests/unit/workflow/backend_selection_test.cc`
- Modify: `tests/unit/workflow/BUILD.bazel`

**Interfaces:**
- Consumes: `AgentNodeBuildSpec.backend` / `.backends` (Task 6); `FakeChatBackend` (Task 2).
- Produces: `proto::AgentDef::ModelSpec::backend` (a `string`); `BuildAgentNode` throwing `AgentflowError` on an unknown name. Task 12's example sets the field.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/workflow/backend_selection_test.cc`:

```cpp
// tests/unit/workflow/backend_selection_test.cc
//
// ModelSpec.backend names a logical backend the host registered. Credentials
// and base URLs live in the host-constructed instance, never in the spec.
#include <memory>
#include <string>
#include <vector>

#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/errors.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "agentflow/workflow/workflow_runner.h"
#include "tests/support/fake_chat_backend.h"

namespace agentflow::workflow {
namespace {

// Minimal two-agent workflow: `local` takes the default backend, `cloud`
// selects a named one.
constexpr char kWorkflowJson[] = R"({
  "name": "backend-selection",
  "version": "1",
  "state": {"fields": {}},
  "agents": {
    "local": {"system_prompt": "local agent"},
    "cloud": {"system_prompt": "cloud agent",
              "model": {"backend": "cloud-big"}}
  },
  "graph": {"nodes": [{"id": "local", "agent": "local"}], "edges": []}
})";

std::shared_ptr<Workflow> LoadWorkflow(const ToolRegistry& tools) {
  auto wf_or = WorkflowLoader::Load(kWorkflowJson, tools);
  EXPECT_TRUE(wf_or.ok()) << wf_or.status().message();
  return *wf_or;
}

AgentNodeBuildSpec MakeSpec(std::shared_ptr<Workflow> wf,
                             std::shared_ptr<ToolRegistry> tools,
                             asio::io_context& io) {
  AgentNodeBuildSpec spec;
  spec.workflow = std::move(wf);
  spec.host_tools = std::move(tools);
  spec.io_ctx = &io;
  return spec;
}

TEST(BackendSelectionTest, EmptyBackendNameUsesTheDefault) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>();
  auto spec = MakeSpec(LoadWorkflow(*tools), tools, io);

  auto fallback = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  spec.backend = fallback;
  spec.agent_name = "local";

  auto built = BuildAgentNode(spec);
  EXPECT_EQ(built.cfg.backend, fallback);
}

TEST(BackendSelectionTest, NamedBackendIsResolvedFromTheMap) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>();
  auto spec = MakeSpec(LoadWorkflow(*tools), tools, io);

  auto fallback = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  auto cloud = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  spec.backend = fallback;
  spec.backends["cloud-big"] = cloud;
  spec.agent_name = "cloud";

  auto built = BuildAgentNode(spec);
  EXPECT_EQ(built.cfg.backend, cloud);
  EXPECT_NE(built.cfg.backend, fallback);
}

TEST(BackendSelectionTest, UnknownBackendNameThrowsRatherThanFallingBack) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>();
  auto spec = MakeSpec(LoadWorkflow(*tools), tools, io);

  // A default IS available — the point is that it must NOT be used. Silently
  // demoting an agent from its intended cloud model to a local one would
  // change answer quality invisibly (design spec §5).
  spec.backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  spec.agent_name = "cloud";  // wants "cloud-big", which is not registered

  EXPECT_THROW(BuildAgentNode(spec), AgentflowError);
}

}  // namespace
}  // namespace agentflow::workflow
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test //tests/unit/workflow:backend_selection_test`
Expected: FAIL — the workflow JSON's `model.backend` key is rejected or
ignored, and `BuildAgentNode` does not throw.

- [ ] **Step 3: Add the proto field**

In `proto/workflow_spec.proto`, extend `ModelSpec` (currently lines 28-31):

```proto
  message ModelSpec {
    int32 max_output_tokens     = 1;
    bool  constrained_tool_calls = 2;

    // Logical name of a host-registered inference backend, e.g. "cloud-big".
    // Empty selects the default backend. Deliberately NOT a base URL, model
    // id, or credential — those live in host code so they never reach a
    // serialized spec, a checkpoint, or a trace. See design spec §5.
    string backend               = 3;
  }
```

- [ ] **Step 4: Parse it in the loader**

In `agentflow/workflow/workflow_loader.cc`, find where `ModelSpec`'s existing
fields are parsed (search for `max_output_tokens`) and add alongside them:

```cpp
    if (jm.contains("backend")) {
      if (!jm["backend"].is_string()) {
        return absl::InvalidArgumentError(
            absl::StrCat("agent '", agent_name, "': model.backend must be a string"));
      }
      model->set_backend(jm["backend"].get<std::string>());
    }
```

Match the surrounding code's variable names — if the local JSON object is not
called `jm` or the message pointer not `model`, use whatever is already there.

- [ ] **Step 5: Resolve and reject in the runner**

In `agentflow/workflow/workflow_runner.cc`, add this file-local helper above
`BuildAgentNode` and replace the `cfg.backend = spec.backend;` line from Task 6
with a call to it:

```cpp
namespace {

// Resolves an agent's inference backend. An empty ModelSpec.backend selects
// the build spec's default; a name must be present in the backends map.
//
// An unknown name throws rather than falling back to the default: silently
// demoting an agent from its intended cloud model to a local one would change
// answer quality invisibly. Design spec §5.
std::shared_ptr<::agentflow::IChatBackend> ResolveBackend(
    const AgentNodeBuildSpec& spec, const proto::AgentDef& agent_def) {
  const std::string& name = agent_def.model().backend();
  if (name.empty()) return spec.backend;
  auto it = spec.backends.find(name);
  if (it == spec.backends.end()) {
    throw AgentflowError("agent '" + spec.agent_name +
                          "' requests backend '" + name +
                          "' which the host did not register");
  }
  return it->second;
}

}  // namespace
```

Add `#include "agentflow/core/errors.h"` if it is not already included.

Use the real accessor for the agent's `AgentDef` — the surrounding code already
looks it up by `spec.agent_name` to read `system_prompt`; reuse that variable
rather than looking it up a second time.

- [ ] **Step 6: Declare the test target**

Append to `tests/unit/workflow/BUILD.bazel`:

```python
cc_test(
    name = "backend_selection_test",
    size = "small",
    srcs = ["backend_selection_test.cc"],
    deps = [
        "//agentflow/core",
        "//agentflow/inference:chat_backend",
        "//agentflow/tools",
        "//agentflow/workflow",
        "//tests/support:fake_chat_backend",
        "@asio",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `bazel test //tests/unit/workflow:backend_selection_test --test_output=all`
Expected: PASS — 3 tests.

- [ ] **Step 8: Correct the design spec**

In `docs/superpowers/specs/2026-08-11-remote-llm-backend-design.md` §5, replace
the bullet reading *"A `ModelSpec.backend` naming a logical backend absent from
the map is a **load-time error**, reported by `workflow_loader` …"* with:

```markdown
- A `ModelSpec.backend` naming a logical backend absent from the map is a
  **build-time error**: `BuildAgentNode` throws `AgentflowError`. Validation
  cannot live in `workflow_loader`, which parses JSON into the proto and has
  no access to the host's backends map. It is not silently resolved to the
  default backend: falling back from an intended cloud model to a local one
  would change answer quality invisibly.
```

- [ ] **Step 9: Run the full suite and commit**

Run: `bazel test //tests/unit/... //tests/integration/...`
Expected: all PASS.

```bash
git add proto/workflow_spec.proto agentflow/workflow/ tests/unit/workflow/ \
        docs/superpowers/specs/2026-08-11-remote-llm-backend-design.md
git commit -m "feat(workflow): per-agent backend selection via ModelSpec.backend

An agent names a host-registered backend by logical name; credentials and
base URLs stay in host code. An unknown name throws instead of falling back to
the default, so an agent never silently drops from its intended cloud model to
a local one.

Corrects design spec 5: validation happens in BuildAgentNode, not
workflow_loader, which has no access to the runtime backends map."
```

---

## Task 8: HTTP parsing primitives (pure, no sockets)

Spec §4, §7.3. Everything in `HttpsClient` that can be tested without a network
is written and tested here first. Task 9 then only has to move bytes.

**Files:**
- Create: `agentflow/net/http_client.h` (interface only)
- Create: `agentflow/net/http_parse.h`
- Create: `agentflow/net/http_parse.cc`
- Create: `tests/unit/net/http_parse_test.cc`
- Modify: `agentflow/net/BUILD.bazel`
- Modify: `tests/unit/net/BUILD.bazel`

**Interfaces:**
- Consumes: `CancelToken` (existing core).
- Produces:
  - `agentflow::net::HttpRequest{url, body, headers}`
  - `agentflow::net::SseHandler = std::function<void(std::string_view)>`
  - `agentflow::net::IHttpClient` with `PostSse(HttpRequest, const SseHandler&, const CancelToken&) -> asio::awaitable<absl::Status>` and `Post(HttpRequest, const CancelToken&) -> asio::awaitable<absl::StatusOr<std::string>>`
  - `agentflow::net::ParseUrl(std::string_view) -> absl::StatusOr<ParsedUrl{host,port,target,tls}>`
  - `agentflow::net::ParseResponseHead(std::string_view) -> absl::StatusOr<ResponseHead{status_code,headers,head_bytes,chunked,content_length}>`
  - `agentflow::net::ChunkedDecoder` with `Feed(std::string_view) -> absl::StatusOr<std::string>`, `complete()`
  - `agentflow::net::SseFramer` with `Feed(std::string_view) -> std::vector<std::string>`, `saw_done()`
  - Task 9 implements `IHttpClient`; Task 12 consumes it.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/net/http_parse_test.cc`:

```cpp
// tests/unit/net/http_parse_test.cc
#include "agentflow/net/http_parse.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace agentflow::net {
namespace {

TEST(ParseUrlTest, HttpsDefaultsToPort443) {
  auto u = ParseUrl("https://api.openai.com/v1/chat/completions");
  ASSERT_TRUE(u.ok()) << u.status().message();
  EXPECT_EQ(u->host, "api.openai.com");
  EXPECT_EQ(u->port, "443");
  EXPECT_EQ(u->target, "/v1/chat/completions");
  EXPECT_TRUE(u->tls);
}

TEST(ParseUrlTest, ExplicitPortAndPlainHttp) {
  auto u = ParseUrl("http://127.0.0.1:11434/v1/chat/completions");
  ASSERT_TRUE(u.ok());
  EXPECT_EQ(u->host, "127.0.0.1");
  EXPECT_EQ(u->port, "11434");
  EXPECT_EQ(u->target, "/v1/chat/completions");
  EXPECT_FALSE(u->tls);
}

TEST(ParseUrlTest, MissingPathBecomesRoot) {
  auto u = ParseUrl("https://example.com");
  ASSERT_TRUE(u.ok());
  EXPECT_EQ(u->target, "/");
}

TEST(ParseUrlTest, RejectsUnsupportedScheme) {
  EXPECT_FALSE(ParseUrl("ftp://example.com/x").ok());
  EXPECT_FALSE(ParseUrl("example.com/x").ok());
}

TEST(ParseResponseHeadTest, ParsesStatusAndHeadersCaseInsensitively) {
  const std::string raw =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "body-starts-here";
  auto h = ParseResponseHead(raw);
  ASSERT_TRUE(h.ok());
  EXPECT_EQ(h->status_code, 200);
  EXPECT_TRUE(h->chunked);
  EXPECT_EQ(raw.substr(h->head_bytes), "body-starts-here");
}

TEST(ParseResponseHeadTest, ReportsContentLengthWhenNotChunked) {
  auto h = ParseResponseHead(
      "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 17\r\n\r\n");
  ASSERT_TRUE(h.ok());
  EXPECT_EQ(h->status_code, 429);
  EXPECT_FALSE(h->chunked);
  EXPECT_EQ(h->content_length, 17);
}

TEST(ParseResponseHeadTest, IncompleteHeadIsNotAnError) {
  // The terminator has not arrived yet — the caller must read more bytes.
  auto h = ParseResponseHead("HTTP/1.1 200 OK\r\nContent-Ty");
  ASSERT_TRUE(h.ok());
  EXPECT_EQ(h->head_bytes, 0u);  // 0 = "not complete yet"
}

TEST(ChunkedDecoderTest, DecodesChunksSplitAcrossFeeds) {
  ChunkedDecoder d;
  auto a = d.Feed("5\r\nhel");
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(*a, "hel");
  auto b = d.Feed("lo\r\n0\r\n\r\n");
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(*b, "lo");
  EXPECT_TRUE(d.complete());
}

TEST(ChunkedDecoderTest, HandlesAChunkSizeLineSplitMidNumber) {
  ChunkedDecoder d;
  auto a = d.Feed("1");
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(*a, "");
  auto b = d.Feed("0\r\n0123456789\r\n0\r\n\r\n");
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(*b, "0123456789");
  EXPECT_TRUE(d.complete());
}

TEST(SseFramerTest, SplitsFramesOnBlankLine) {
  SseFramer f;
  auto got = f.Feed("data: {\"a\":1}\n\ndata: {\"b\":2}\n\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})", R"({"b":2})"}));
  EXPECT_FALSE(f.saw_done());
}

TEST(SseFramerTest, HoldsAPartialFrameUntilItCompletes) {
  SseFramer f;
  EXPECT_TRUE(f.Feed("data: {\"a\":").empty());
  auto got = f.Feed("1}\n\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})"}));
}

TEST(SseFramerTest, DoneSentinelIsConsumedNotDelivered) {
  SseFramer f;
  auto got = f.Feed("data: {\"a\":1}\n\ndata: [DONE]\n\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})"}));
  EXPECT_TRUE(f.saw_done());
}

TEST(SseFramerTest, IgnoresCommentsAndEventLines) {
  SseFramer f;
  auto got = f.Feed(": keep-alive\n\nevent: ping\ndata: {\"a\":1}\n\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})"}));
}

TEST(SseFramerTest, ToleratesCrLfLineEndings) {
  SseFramer f;
  auto got = f.Feed("data: {\"a\":1}\r\n\r\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})"}));
}

}  // namespace
}  // namespace agentflow::net
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test //tests/unit/net:http_parse_test`
Expected: FAIL — `agentflow/net/http_parse.h` does not exist.

- [ ] **Step 3: Write the client interface header**

Create `agentflow/net/http_client.h`:

```cpp
// agentflow/net/http_client.h
//
// The HTTP surface the OpenAI backend consumes. Kept separate from the
// implementation so tests can inject a fake and exercise request building and
// response mapping with no network.
#ifndef AGENTFLOW_NET_HTTP_CLIENT_H_
#define AGENTFLOW_NET_HTTP_CLIENT_H_

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"

namespace agentflow::net {

struct HttpRequest {
  std::string url;  // http(s)://host[:port]/path
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

// One SSE frame's data payload, with "data: " already stripped. The [DONE]
// sentinel is never delivered — it terminates the stream instead.
using SseHandler = std::function<void(std::string_view data)>;

class IHttpClient {
 public:
  virtual ~IHttpClient() = default;

  // POSTs and streams the response, invoking `on_event` per SSE frame.
  // Returns OK once the stream ends cleanly. A non-2xx status is reported as
  // a non-OK Status whose message contains the response body (callers must
  // scrub credentials before logging — the body itself never carries them).
  virtual asio::awaitable<absl::Status> PostSse(
      HttpRequest req, const SseHandler& on_event,
      const CancelToken& cancel) = 0;

  // POSTs and returns the whole response body.
  virtual asio::awaitable<absl::StatusOr<std::string>> Post(
      HttpRequest req, const CancelToken& cancel) = 0;
};

}  // namespace agentflow::net
#endif  // AGENTFLOW_NET_HTTP_CLIENT_H_
```

- [ ] **Step 4: Write the parser header**

Create `agentflow/net/http_parse.h`:

```cpp
// agentflow/net/http_parse.h
//
// Pure HTTP/1.1 and SSE parsing. No sockets, no TLS, no io_context — every
// entity here is directly unit-testable, which is where the bulk of the
// client's correctness risk lives.
#ifndef AGENTFLOW_NET_HTTP_PARSE_H_
#define AGENTFLOW_NET_HTTP_PARSE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/statusor.h>

namespace agentflow::net {

struct ParsedUrl {
  std::string host;
  std::string port;    // always populated: "443" / "80" / explicit
  std::string target;  // path + query; "/" when the URL has none
  bool tls = false;
};

// Accepts http:// and https:// only. Any other scheme, or a missing scheme,
// is an InvalidArgumentError.
absl::StatusOr<ParsedUrl> ParseUrl(std::string_view url);

struct ResponseHead {
  int status_code = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercased
  // Bytes consumed by the status line + headers + terminator. ZERO means the
  // head is not complete yet and the caller must read more.
  std::size_t head_bytes = 0;
  bool chunked = false;
  std::int64_t content_length = -1;  // -1 when absent
};

// Parses as much of a response head as `buf` contains. An incomplete head is
// NOT an error — it returns head_bytes == 0. A malformed status line is.
absl::StatusOr<ResponseHead> ParseResponseHead(std::string_view buf);

// Incremental HTTP/1.1 chunked-transfer decoder. Feed raw body bytes as they
// arrive; each call returns whatever decoded payload became available.
class ChunkedDecoder {
 public:
  absl::StatusOr<std::string> Feed(std::string_view bytes);
  // True once the terminating zero-length chunk has been seen.
  bool complete() const { return complete_; }

 private:
  std::string buf_;         // undecoded remainder
  std::size_t remaining_ = 0;  // bytes left in the current chunk
  bool in_chunk_ = false;
  bool complete_ = false;
};

// Incremental Server-Sent Events framer. Feed raw (already de-chunked) bytes;
// each call returns the `data:` payloads of every frame that completed.
//
// Comments (": ..."), `event:` lines and other field names are ignored — the
// OpenAI stream only uses `data:`. The literal payload "[DONE]" is consumed
// and reported through saw_done() rather than delivered.
class SseFramer {
 public:
  std::vector<std::string> Feed(std::string_view bytes);
  bool saw_done() const { return saw_done_; }

 private:
  std::string buf_;
  bool saw_done_ = false;
};

}  // namespace agentflow::net
#endif  // AGENTFLOW_NET_HTTP_PARSE_H_
```

- [ ] **Step 5: Write the parser implementation**

Create `agentflow/net/http_parse.cc`:

```cpp
// agentflow/net/http_parse.cc
#include "agentflow/net/http_parse.h"

#include <algorithm>
#include <cctype>
#include <charconv>

#include <absl/status/status.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>

namespace agentflow::net {
namespace {

// Splits `s` at the first occurrence of `sep`. Returns npos-safe halves.
std::pair<std::string_view, std::string_view> SplitOnce(std::string_view s,
                                                         std::string_view sep) {
  const auto pos = s.find(sep);
  if (pos == std::string_view::npos) return {s, {}};
  return {s.substr(0, pos), s.substr(pos + sep.size())};
}

std::string_view TrimAscii(std::string_view s) {
  while (!s.empty() && absl::ascii_isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && absl::ascii_isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

}  // namespace

absl::StatusOr<ParsedUrl> ParseUrl(std::string_view url) {
  ParsedUrl out;
  std::string_view rest;
  if (url.starts_with("https://")) {
    out.tls = true;
    out.port = "443";
    rest = url.substr(8);
  } else if (url.starts_with("http://")) {
    out.tls = false;
    out.port = "80";
    rest = url.substr(7);
  } else {
    return absl::InvalidArgumentError(
        absl::StrCat("unsupported URL scheme: ", url));
  }

  const auto slash = rest.find('/');
  std::string_view authority = rest.substr(0, slash);
  out.target = slash == std::string_view::npos
                    ? "/"
                    : std::string(rest.substr(slash));
  if (authority.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("URL has no host: ", url));
  }

  const auto colon = authority.rfind(':');
  if (colon != std::string_view::npos) {
    out.host = std::string(authority.substr(0, colon));
    out.port = std::string(authority.substr(colon + 1));
  } else {
    out.host = std::string(authority);
  }
  if (out.host.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("URL has no host: ", url));
  }
  return out;
}

absl::StatusOr<ResponseHead> ParseResponseHead(std::string_view buf) {
  ResponseHead head;
  const auto term = buf.find("\r\n\r\n");
  if (term == std::string_view::npos) return head;  // incomplete; head_bytes==0

  std::string_view h = buf.substr(0, term);
  auto [status_line, header_block] = SplitOnce(h, "\r\n");

  // "HTTP/1.1 200 OK"
  auto [version, after_version] = SplitOnce(status_line, " ");
  if (!version.starts_with("HTTP/")) {
    return absl::InvalidArgumentError("malformed HTTP status line");
  }
  auto [code_str, _reason] = SplitOnce(after_version, " ");
  int code = 0;
  const auto res = std::from_chars(code_str.data(),
                                    code_str.data() + code_str.size(), code);
  if (res.ec != std::errc{} || code < 100 || code > 599) {
    return absl::InvalidArgumentError("malformed HTTP status code");
  }
  head.status_code = code;

  std::string_view remaining = header_block;
  while (!remaining.empty()) {
    auto [line, tail] = SplitOnce(remaining, "\r\n");
    remaining = tail;
    if (line.empty()) continue;
    auto [name, value] = SplitOnce(line, ":");
    std::string lname = absl::AsciiStrToLower(TrimAscii(name));
    std::string lvalue(TrimAscii(value));
    if (lname == "transfer-encoding" &&
        absl::AsciiStrToLower(lvalue).find("chunked") != std::string::npos) {
      head.chunked = true;
    } else if (lname == "content-length") {
      std::int64_t n = 0;
      if (std::from_chars(lvalue.data(), lvalue.data() + lvalue.size(), n).ec ==
          std::errc{}) {
        head.content_length = n;
      }
    }
    head.headers.emplace_back(std::move(lname), std::move(lvalue));
  }

  head.head_bytes = term + 4;
  return head;
}

absl::StatusOr<std::string> ChunkedDecoder::Feed(std::string_view bytes) {
  buf_.append(bytes);
  std::string out;
  std::size_t pos = 0;

  for (;;) {
    if (complete_) break;
    if (in_chunk_) {
      const std::size_t avail = buf_.size() - pos;
      const std::size_t take = std::min(avail, remaining_);
      out.append(buf_, pos, take);
      pos += take;
      remaining_ -= take;
      if (remaining_ > 0) break;  // need more bytes
      // Consume the CRLF that terminates the chunk body.
      if (buf_.size() - pos < 2) break;
      pos += 2;
      in_chunk_ = false;
      continue;
    }
    // Reading a chunk-size line.
    const auto eol = buf_.find("\r\n", pos);
    if (eol == std::string::npos) break;  // size line incomplete
    std::string_view size_line(buf_.data() + pos, eol - pos);
    // Strip any chunk extension (";name=value").
    size_line = SplitOnce(size_line, ";").first;
    std::size_t n = 0;
    const auto res = std::from_chars(size_line.data(),
                                      size_line.data() + size_line.size(), n, 16);
    if (res.ec != std::errc{}) {
      return absl::InvalidArgumentError("malformed chunk size");
    }
    pos = eol + 2;
    if (n == 0) {
      complete_ = true;
      break;
    }
    remaining_ = n;
    in_chunk_ = true;
  }

  buf_.erase(0, pos);
  return out;
}

std::vector<std::string> SseFramer::Feed(std::string_view bytes) {
  buf_.append(bytes);
  std::vector<std::string> out;

  for (;;) {
    // A frame ends at a blank line. Accept both LF and CRLF forms.
    std::size_t end = buf_.find("\n\n");
    std::size_t sep_len = 2;
    const std::size_t crlf = buf_.find("\r\n\r\n");
    if (crlf != std::string::npos && (end == std::string::npos || crlf < end)) {
      end = crlf;
      sep_len = 4;
    }
    if (end == std::string::npos) break;

    std::string_view frame(buf_.data(), end);
    std::string payload;
    while (!frame.empty()) {
      auto [line, tail] = SplitOnce(frame, "\n");
      frame = tail;
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.empty() || line.front() == ':') continue;  // blank or comment
      auto [name, value] = SplitOnce(line, ":");
      if (name != "data") continue;  // ignore event:, id:, retry:
      // Per the SSE spec a single leading space after the colon is stripped.
      if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
      if (!payload.empty()) payload.push_back('\n');
      payload.append(value);
    }
    buf_.erase(0, end + sep_len);

    if (payload.empty()) continue;
    if (payload == "[DONE]") {
      saw_done_ = true;
      continue;
    }
    out.push_back(std::move(payload));
  }
  return out;
}

}  // namespace agentflow::net
```

- [ ] **Step 6: Add the Bazel targets**

Append to `agentflow/net/BUILD.bazel`:

```python
cc_library(
    name = "http_client",
    hdrs = ["http_client.h"],
    deps = [
        "//agentflow/core",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@asio",
    ],
)

cc_library(
    name = "http_parse",
    srcs = ["http_parse.cc"],
    hdrs = ["http_parse.h"],
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)
```

Append to `tests/unit/net/BUILD.bazel`:

```python
cc_test(
    name = "http_parse_test",
    size = "small",
    srcs = ["http_parse_test.cc"],
    deps = [
        "//agentflow/net:http_parse",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `bazel test //tests/unit/net:http_parse_test --test_output=all`
Expected: PASS — 13 tests.

- [ ] **Step 8: Commit**

```bash
git add agentflow/net/ tests/unit/net/
git commit -m "feat(net): pure HTTP/1.1 and SSE parsing primitives

URL parsing, response-head parsing, incremental chunked decoding and SSE
framing, all socket-free and directly unit-tested. Adds the IHttpClient
interface so the OpenAI backend can be tested against a fake."
```

---
