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
- **Token delivery is back-pressured, never lossy.** `TokenSink` and
  `net::SseHandler` both return `asio::awaitable<void>` and are always
  `co_await`ed at their call sites. A consumer that is not keeping up slows the
  decode loop down; it never silently loses a token. This deliberately replaces
  the pre-existing `try_send` in `sub_agent_runtime.cc:236` — the codebase was
  inconsistent (`AgentNode` awaited its channel send, `SubAgentRuntime` dropped
  on a full channel) and this plan settles it on the awaiting form everywhere.
  If a call site cannot `co_await`, that is a signal the signature is wrong —
  do not reach for `try_send` to work around it.

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
        // TokenSink returns an awaitable, so the sink is a coroutine lambda.
        [&](std::string_view d) -> asio::awaitable<void> {
          seen.emplace_back(d);
          co_return;
        },
        cancel);
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

// One text delta from the model.
//
// Returns an awaitable and is ALWAYS co_awaited by its caller, so a consumer
// that is not keeping up applies back-pressure to the decode loop instead of
// having its tokens dropped. An empty (falsy) sink means "nobody wants
// deltas" — check it before calling.
using TokenSink =
    std::function<asio::awaitable<void>(std::string_view delta)>;

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
      for (const auto& d : deltas_) co_await on_token(d);
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
        [&](std::string_view d) -> asio::awaitable<void> {
          deltas.emplace_back(d);
          co_return;
        },
        cancel);
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
      // co_await, so a slow consumer back-pressures the decode loop.
      for (size_t i = before; i < assembler.text_deltas().size(); ++i) {
        co_await on_token(assembler.text_deltas()[i]);
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
      sink = [this, &emit](std::string_view delta)
                 -> asio::awaitable<void> {
        emit.EmitToken(Id(), delta);
        if (cfg_.token_channel) {
          // Back-pressured send: a consumer that is not keeping up slows the
          // decode loop rather than losing tokens. as_tuple so a closed
          // channel (consumer gone) yields an error instead of throwing — we
          // simply stop forwarding in that case. This is the same behaviour
          // the pre-refactor AgentNode had; it is preserved exactly.
          auto [ec] = co_await cfg_.token_channel->async_send(
              asio::error_code{}, std::string(delta),
              asio::as_tuple(asio::use_awaitable));
          (void)ec;
        }
        co_return;
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
    sink = [this, &emit](std::string_view delta) -> asio::awaitable<void> {
      emit.EmitToken(Id(), delta);
      co_return;
    };
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

**Also in `sub_agent_runtime.cc`, convert the token-channel sink at lines
233-239 to the back-pressured form.** It currently drops deltas on a full
channel, which the token-delivery Global Constraint forbids:

```cpp
  TokenSink on_token;
  if (ctx.token_channel != nullptr) {
    auto* ch = ctx.token_channel;
    on_token = [ch](std::string_view delta) -> asio::awaitable<void> {
      // Back-pressured: a slow consumer slows the sub-agent's decode loop
      // rather than losing tokens. as_tuple so a closed channel yields an
      // error instead of throwing.
      auto [ec] = co_await ch->async_send(asio::error_code{},
                                           std::string(delta),
                                           asio::as_tuple(asio::use_awaitable));
      (void)ec;
      co_return;
    };
  }
```

Add `#include <asio/as_tuple.hpp>` and `#include <asio/use_awaitable.hpp>` to
that file if they are not already present.

**One test edit is required here**, and it is the only place in this plan
where `sub_agent_runtime_test.cc` changes. Its streaming fake (around
`sub_agent_runtime_test.cc:150-165`) calls `on_token(...)` synchronously;
`TokenSink` now returns an awaitable, so that call becomes
`co_await on_token(...)` and the enclosing lambda becomes a coroutine. This is
a mechanical signature change — **the `SendFn` contract and `RunAsync`'s
behaviour still do not change**, and every assertion in the file stays as it
is. If you find yourself changing an assertion, stop: something else is wrong.

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
Expected: all PASS, with `sub_agent_runtime_test` carrying **only** the
`co_await on_token(...)` edit described in Step 4 — no assertion changes.

If it fails for any other reason, the `ConversationFactory` signature drifted.
Fix the alias in `sub_agent_runtime.h` rather than the test.

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
  - `agentflow::net::SseHandler = std::function<asio::awaitable<void>(std::string_view)>`
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
//
// Returns an awaitable and is always co_awaited by the client's read loop, so
// a slow consumer back-pressures the socket read rather than losing frames.
using SseHandler =
    std::function<asio::awaitable<void>(std::string_view data)>;

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
                    ? std::string("/")
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

## Task 9: `HttpsClient` — sockets and TLS

Spec §4.1, §4.2, §7.3. The only file in this plan that touches the network.
All parsing is already done and tested (Task 8), so this task is transport
only.

**Files:**
- Create: `agentflow/net/https_client.h`
- Create: `agentflow/net/https_client.cc`
- Create: `tests/unit/net/https_client_integration_test.cc`
- Modify: `agentflow/net/BUILD.bazel`
- Modify: `tests/unit/net/BUILD.bazel`

**Interfaces:**
- Consumes: `IHttpClient`, `HttpRequest`, `SseHandler` (Task 8); `ParseUrl`, `ParseResponseHead`, `ChunkedDecoder`, `SseFramer` (Task 8).
- Produces: `agentflow::net::HttpsClientOptions{ca_path, connect_timeout, read_timeout}` and `agentflow::net::HttpsClient(asio::io_context&, HttpsClientOptions)` implementing `IHttpClient`. Task 12 constructs one.

**Task 1 selected Variant A (`asio::ssl`)** — its probe compiled, linked
against BoringSSL's `libssl`/`libcrypto` (verified with `ldd`, not system
OpenSSL), and passed all three cases including loading the system CA bundle.
Step 2 below is the complete Variant A implementation. There is no Variant B
to write.

**Carried forward from Task 1's report:** that probe ran host-side only
(x86_64 glibc). The Android/NDK cross-compile was not probed, and Android has
no `/etc/ssl/certs/ca-certificates.crt` — it uses the hashed CA directory
`/system/etc/security/cacerts/`. That is why `ca_path` accepts a directory as
well as a file, and why `IsDirectory` decides between `add_verify_path` and
`load_verify_file`. Do not hard-code the desktop bundle path anywhere in
`https_client.cc`; it belongs only in host code and tests.

- [ ] **Step 1: Write the header**

Create `agentflow/net/https_client.h`:

```cpp
// agentflow/net/https_client.h
#ifndef AGENTFLOW_NET_HTTPS_CLIENT_H_
#define AGENTFLOW_NET_HTTPS_CLIENT_H_

#include <chrono>
#include <memory>
#include <string>

#include <asio/io_context.hpp>

#include "agentflow/net/http_client.h"

namespace agentflow::net {

struct HttpsClientOptions {
  // Either a CA bundle FILE (desktop, e.g.
  // /etc/ssl/certs/ca-certificates.crt) or a hashed CA DIRECTORY (Android,
  // /system/etc/security/cacerts/). Both forms are accepted; the client
  // stats the path to decide. Required for https:// URLs — there is
  // deliberately no option to skip verification.
  std::string ca_path;

  std::chrono::milliseconds connect_timeout{10'000};
  // Idle timeout BETWEEN reads, not a whole-response deadline: a slow model
  // that streams steadily must not be killed mid-answer.
  std::chrono::milliseconds read_timeout{60'000};
};

// Minimal HTTP/1.1 client. POST only. Supports chunked and SSE bodies.
//
// Deliberately NOT supported (design spec §4): redirect following, connection
// pooling (one connection per request), HTTP/2.
class HttpsClient : public IHttpClient {
 public:
  HttpsClient(asio::io_context& io, HttpsClientOptions opts);
  ~HttpsClient() override;

  asio::awaitable<absl::Status> PostSse(HttpRequest req,
                                         const SseHandler& on_event,
                                         const CancelToken& cancel) override;

  asio::awaitable<absl::StatusOr<std::string>> Post(
      HttpRequest req, const CancelToken& cancel) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace agentflow::net
#endif  // AGENTFLOW_NET_HTTPS_CLIENT_H_
```

- [ ] **Step 2: Implement the transport**

Create `agentflow/net/https_client.cc`. Structure both variants the same way —
one `Impl` exposing a `Connect` / `Write` / `ReadSome` / `Close` quartet, with
`PostSse` and `Post` written once on top of it:

```cpp
// agentflow/net/https_client.cc
#include "agentflow/net/https_client.h"

#include <sys/stat.h>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "agentflow/net/http_parse.h"

// VARIANT A only:
#include <asio/ssl.hpp>

namespace agentflow::net {
namespace {

// Builds the request bytes. Connection: close because there is no pooling —
// the server closing the socket is our end-of-body signal for non-chunked
// responses.
std::string BuildRequestBytes(const ParsedUrl& url, const HttpRequest& req,
                               bool sse) {
  std::string out = absl::StrCat("POST ", url.target, " HTTP/1.1\r\n",
                                  "Host: ", url.host, "\r\n",
                                  "Connection: close\r\n",
                                  "Content-Length: ", req.body.size(), "\r\n");
  if (sse) absl::StrAppend(&out, "Accept: text/event-stream\r\n");
  for (const auto& [k, v] : req.headers) {
    absl::StrAppend(&out, k, ": ", v, "\r\n");
  }
  absl::StrAppend(&out, "\r\n", req.body);
  return out;
}

bool IsDirectory(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace
```

Continue the same file with the status mapping and the connection. **This is
the Variant A (`asio::ssl`) implementation, which Task 1 selected** — its probe
compiled, linked against BoringSSL's libssl/libcrypto, and passed all three
cases. Do not write a Variant B fallback.

```cpp
// The absl codes Task 12's retry policy keys on. Design spec §6.
absl::StatusCode MapHttpStatus(int code) {
  if (code == 401 || code == 403) return absl::StatusCode::kPermissionDenied;
  if (code == 429) return absl::StatusCode::kResourceExhausted;
  if (code >= 500) return absl::StatusCode::kUnavailable;
  if (code >= 400) return absl::StatusCode::kInvalidArgument;
  return absl::StatusCode::kUnknown;
}

// One request's connection. Held by shared_ptr so the cancellation hook stays
// valid for as long as the socket does.
//
// A single ssl::stream is used for both schemes: for https the TLS layer is
// driven directly, for plain http the same object's next_layer() (the bare
// tcp::socket) is used and no handshake happens.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(asio::io_context& io, HttpsClientOptions opts)
      : opts_(std::move(opts)),
        ssl_ctx_(asio::ssl::context::tls_client),
        stream_(io, ssl_ctx_),
        deadline_(io) {}

  // Idempotent and safe from any thread. Pending async ops then complete with
  // operation_aborted, which is how both cancellation and timeout unblock a
  // stalled co_await.
  void Close() {
    asio::error_code ignored;
    stream_.next_layer().close(ignored);
  }

  asio::awaitable<absl::Status> Connect(const ParsedUrl& url) {
    tls_ = url.tls;

    if (tls_) {
      if (opts_.ca_path.empty()) {
        co_return absl::InvalidArgumentError(
            "ca_path is required for https:// URLs; verification is mandatory");
      }
      asio::error_code ec;
      ssl_ctx_.set_verify_mode(asio::ssl::verify_peer, ec);
      if (ec) {
        co_return absl::InternalError(
            absl::StrCat("set_verify_mode failed: ", ec.message()));
      }
      // A bundle FILE on desktop, a hashed CA DIRECTORY on Android.
      if (IsDirectory(opts_.ca_path)) {
        ssl_ctx_.add_verify_path(opts_.ca_path, ec);
      } else {
        ssl_ctx_.load_verify_file(opts_.ca_path, ec);
      }
      if (ec) {
        co_return absl::InvalidArgumentError(absl::StrCat(
            "cannot load ca_path '", opts_.ca_path, "': ", ec.message()));
      }
      stream_.set_verify_callback(asio::ssl::host_name_verification(url.host),
                                   ec);
      if (ec) {
        co_return absl::InternalError(
            absl::StrCat("set_verify_callback failed: ", ec.message()));
      }
      // SNI is mandatory at every cloud endpoint and must be set BEFORE the
      // handshake. Unlike the asio calls above this one reports through the
      // raw OpenSSL API, so it is checked separately.
      if (SSL_set_tlsext_host_name(stream_.native_handle(),
                                    url.host.c_str()) != 1) {
        co_return absl::InternalError("failed to set TLS SNI hostname");
      }
    }

    auto executor = co_await asio::this_coro::executor;
    asio::ip::tcp::resolver resolver(executor);

    ArmDeadline(opts_.connect_timeout);
    auto [rec, endpoints] = co_await resolver.async_resolve(
        url.host, url.port, asio::as_tuple(asio::use_awaitable));
    if (rec) {
      DisarmDeadline();
      co_return absl::UnavailableError(
          absl::StrCat("cannot resolve ", url.host, ": ", rec.message()));
    }

    auto [cec, endpoint] = co_await asio::async_connect(
        stream_.next_layer(), endpoints, asio::as_tuple(asio::use_awaitable));
    if (cec) {
      DisarmDeadline();
      co_return absl::UnavailableError(
          absl::StrCat("cannot connect to ", url.host, ":", url.port, ": ",
                        cec.message()));
    }

    if (tls_) {
      auto [hec] = co_await stream_.async_handshake(
          asio::ssl::stream_base::client, asio::as_tuple(asio::use_awaitable));
      if (hec) {
        DisarmDeadline();
        co_return absl::UnavailableError(
            absl::StrCat("TLS handshake with ", url.host, " failed: ",
                          hec.message()));
      }
    }
    DisarmDeadline();
    co_return absl::OkStatus();
  }

  asio::awaitable<absl::Status> WriteAll(const std::string& bytes) {
    ArmDeadline(opts_.read_timeout);
    auto buf = asio::buffer(bytes);
    auto [ec, n] =
        tls_ ? co_await asio::async_write(stream_, buf,
                                           asio::as_tuple(asio::use_awaitable))
             : co_await asio::async_write(stream_.next_layer(), buf,
                                           asio::as_tuple(asio::use_awaitable));
    DisarmDeadline();
    (void)n;
    if (ec) {
      co_return absl::UnavailableError(
          absl::StrCat("request write failed: ", ec.message()));
    }
    co_return absl::OkStatus();
  }

  // Reads whatever is available. Returns an empty string with ok() when the
  // peer closed cleanly — that is end-of-body, not an error, because we send
  // Connection: close.
  asio::awaitable<absl::StatusOr<std::string>> ReadSome() {
    std::array<char, 8192> buf{};
    ArmDeadline(opts_.read_timeout);
    auto asio_buf = asio::buffer(buf);
    auto [ec, n] =
        tls_ ? co_await stream_.async_read_some(
                   asio_buf, asio::as_tuple(asio::use_awaitable))
             : co_await stream_.next_layer().async_read_some(
                   asio_buf, asio::as_tuple(asio::use_awaitable));
    DisarmDeadline();

    if (!ec) co_return std::string(buf.data(), n);
    // Both are ordinary end-of-stream for a Connection: close response.
    if (ec == asio::error::eof ||
        ec == asio::ssl::error::stream_truncated) {
      co_return std::string{};
    }
    co_return absl::UnavailableError(
        absl::StrCat("response read failed: ", ec.message()));
  }

 private:
  // Watchdog: on expiry the socket is closed, so the in-flight async op
  // completes with operation_aborted instead of hanging forever. read_timeout
  // is an IDLE timeout — it is re-armed per read, so a model that streams
  // steadily for minutes is never killed mid-answer.
  void ArmDeadline(std::chrono::milliseconds d) {
    deadline_.expires_after(d);
    auto self = weak_from_this();
    deadline_.async_wait([self](const asio::error_code& ec) {
      if (ec) return;  // cancelled, i.e. the operation finished in time
      if (auto c = self.lock()) c->Close();
    });
  }
  void DisarmDeadline() { deadline_.cancel(); }

  HttpsClientOptions opts_;
  asio::ssl::context ssl_ctx_;                       // declared before stream_
  asio::ssl::stream<asio::ip::tcp::socket> stream_;
  asio::steady_timer deadline_;
  bool tls_ = false;
};

}  // namespace

class HttpsClient::Impl {
 public:
  Impl(asio::io_context& io, HttpsClientOptions opts)
      : io_(io), opts_(std::move(opts)) {}

  // One driver for both entry points. Exactly one of `on_event` / `out_body`
  // is non-null.
  asio::awaitable<absl::Status> Run(HttpRequest req, const SseHandler* on_event,
                                     std::string* out_body,
                                     const CancelToken& cancel) {
    auto url = ParseUrl(req.url);
    if (!url.ok()) co_return url.status();

    auto conn = std::make_shared<Connection>(io_, opts_);
    // A cancel closes the socket, so the in-flight co_await returns an error
    // which the checks below convert to Cancelled.
    cancel.OnCancel([conn]() { conn->Close(); });

    if (auto s = co_await conn->Connect(*url); !s.ok()) {
      co_return cancel.IsCancelled() ? absl::CancelledError("cancelled") : s;
    }
    if (auto s = co_await conn->WriteAll(
            BuildRequestBytes(*url, req, /*sse=*/on_event != nullptr));
        !s.ok()) {
      co_return cancel.IsCancelled() ? absl::CancelledError("cancelled") : s;
    }

    // Read the response head.
    std::string raw;
    ResponseHead head;
    for (;;) {
      auto parsed = ParseResponseHead(raw);
      if (!parsed.ok()) co_return parsed.status();
      if (parsed->head_bytes != 0) {
        head = *std::move(parsed);
        break;
      }
      auto chunk = co_await conn->ReadSome();
      if (!chunk.ok()) {
        co_return cancel.IsCancelled() ? absl::CancelledError("cancelled")
                                        : chunk.status();
      }
      if (chunk->empty()) {
        co_return absl::UnavailableError(
            "connection closed before a complete response head arrived");
      }
      raw.append(*chunk);
    }

    std::string body_bytes = raw.substr(head.head_bytes);

    // Reject non-2xx before streaming anything. The error body is bounded —
    // 8 KiB covers every provider's error JSON.
    if (head.status_code < 200 || head.status_code >= 300) {
      constexpr size_t kMaxErrorBody = 8192;
      while (body_bytes.size() < kMaxErrorBody) {
        auto chunk = co_await conn->ReadSome();
        if (!chunk.ok() || chunk->empty()) break;
        body_bytes.append(*chunk);
      }
      co_return absl::Status(
          MapHttpStatus(head.status_code),
          absl::StrCat("HTTP ", head.status_code, ": ", body_bytes));
    }

    ChunkedDecoder chunked;
    SseFramer framer;

    // Feeds one slab of raw body bytes onward. Returns false to stop reading.
    auto consume = [&](std::string_view slab) -> asio::awaitable<absl::Status> {
      std::string decoded;
      if (head.chunked) {
        auto d = chunked.Feed(slab);
        if (!d.ok()) co_return d.status();
        decoded = *std::move(d);
      } else {
        decoded = std::string(slab);
      }
      if (out_body) {
        out_body->append(decoded);
        co_return absl::OkStatus();
      }
      for (const auto& payload : framer.Feed(decoded)) {
        // co_await: a slow consumer back-pressures the socket read rather
        // than having frames dropped.
        co_await (*on_event)(payload);
      }
      co_return absl::OkStatus();
    };

    if (auto s = co_await consume(body_bytes); !s.ok()) co_return s;

    for (;;) {
      if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");
      if (on_event && framer.saw_done()) break;
      if (out_body && head.content_length >= 0 &&
          out_body->size() >= static_cast<size_t>(head.content_length)) {
        break;
      }
      if (head.chunked && chunked.complete()) break;

      auto chunk = co_await conn->ReadSome();
      if (!chunk.ok()) {
        co_return cancel.IsCancelled() ? absl::CancelledError("cancelled")
                                        : chunk.status();
      }
      if (chunk->empty()) break;  // peer closed: end of body
      if (auto s = co_await consume(*chunk); !s.ok()) co_return s;
    }

    conn->Close();
    co_return absl::OkStatus();
  }

 private:
  asio::io_context& io_;
  HttpsClientOptions opts_;
};

HttpsClient::HttpsClient(asio::io_context& io, HttpsClientOptions opts)
    : impl_(std::make_unique<Impl>(io, std::move(opts))) {}

HttpsClient::~HttpsClient() = default;

asio::awaitable<absl::Status> HttpsClient::PostSse(HttpRequest req,
                                                    const SseHandler& on_event,
                                                    const CancelToken& cancel) {
  co_return co_await impl_->Run(std::move(req), &on_event, nullptr, cancel);
}

asio::awaitable<absl::StatusOr<std::string>> HttpsClient::Post(
    HttpRequest req, const CancelToken& cancel) {
  std::string body;
  auto status = co_await impl_->Run(std::move(req), nullptr, &body, cancel);
  if (!status.ok()) co_return status;
  co_return body;
}

}  // namespace agentflow::net
```

Add includes as the compiler requires them — at minimum `<array>`,
`<chrono>`, `<memory>`, `asio/steady_timer.hpp`, `asio/this_coro.hpp`,
`asio/as_tuple.hpp`, and `asio/ssl/host_name_verification.hpp` if
`asio/ssl.hpp` does not already pull it in.

Note `asio::ssl::error::stream_truncated` is treated as a clean end of body,
not an error: servers routinely close without a TLS close_notify after a
`Connection: close` response, and treating that as a failure would break every
non-chunked read.

- [ ] **Step 3: Write the opt-in integration test**

Socket and TLS behaviour cannot be asserted in an offline CI environment, so
this test skips unless an endpoint is supplied.

Create `tests/unit/net/https_client_integration_test.cc`:

```cpp
// tests/unit/net/https_client_integration_test.cc
//
// Opt-in. Set AGENTFLOW_TEST_HTTP_URL to an endpoint that accepts a POST and
// returns a body — a local Ollama (http://127.0.0.1:11434/api/tags) exercises
// the plain path; any https:// URL exercises TLS. Skipped by default.
#include "agentflow/net/https_client.h"

#include <cstdlib>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"

namespace agentflow::net {
namespace {

TEST(HttpsClientIntegrationTest, PostReturnsABody) {
  const char* url = std::getenv("AGENTFLOW_TEST_HTTP_URL");
  if (!url) GTEST_SKIP() << "AGENTFLOW_TEST_HTTP_URL not set";

  asio::io_context io;
  HttpsClientOptions opts;
  opts.ca_path = "/etc/ssl/certs/ca-certificates.crt";
  HttpsClient client(io, opts);

  HttpRequest req;
  req.url = url;
  req.body = "{}";
  req.headers = {{"Content-Type", "application/json"}};

  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await client.Post(req, cancel.Token());
      },
      asio::use_future);
  io.run();

  auto body = fut.get();
  ASSERT_TRUE(body.ok()) << body.status().message();
  EXPECT_FALSE(body->empty());
}

TEST(HttpsClientIntegrationTest, RejectsUnsupportedScheme) {
  // Runs everywhere: URL validation needs no network.
  asio::io_context io;
  HttpsClient client(io, HttpsClientOptions{});

  HttpRequest req;
  req.url = "ftp://example.com/x";

  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await client.Post(req, cancel.Token());
      },
      asio::use_future);
  io.run();

  auto r = fut.get();
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace agentflow::net
```

- [ ] **Step 4: Add the Bazel targets**

Append to `agentflow/net/BUILD.bazel`:

```python
cc_library(
    name = "https_client",
    srcs = ["https_client.cc"],
    hdrs = ["https_client.h"],
    deps = [
        ":http_client",
        ":http_parse",
        "//agentflow/core",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@asio",
        "@boringssl//:crypto",
        "@boringssl//:ssl",
    ],
)
```

Append to `tests/unit/net/BUILD.bazel`:

```python
cc_test(
    name = "https_client_integration_test",
    size = "small",
    srcs = ["https_client_integration_test.cc"],
    deps = [
        "//agentflow/core",
        "//agentflow/net:https_client",
        "@abseil-cpp//absl/status:statusor",
        "@asio",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 5: Run the tests**

Run: `bazel test //tests/unit/net:https_client_integration_test --test_output=all`
Expected: PASS — 1 run (`RejectsUnsupportedScheme`), 1 skipped.

Then, if a local Ollama or any reachable endpoint is available, verify the real
path once by hand:

```bash
AGENTFLOW_TEST_HTTP_URL=https://example.com \
  bazel test //tests/unit/net:https_client_integration_test \
  --test_output=all --test_env=AGENTFLOW_TEST_HTTP_URL
```

Expected: both PASS. **Do not skip this manual check** — it is the only
verification that the handshake, CA loading and SNI actually work end to end.

- [ ] **Step 6: Commit**

```bash
git add agentflow/net/ tests/unit/net/
git commit -m "feat(net): HttpsClient — POST with chunked/SSE bodies over TLS

asio + BoringSSL, no new third-party dependency. One connection per request:
no pooling, no redirects, no HTTP/2, all deliberate (design spec 4).
Certificate verification is mandatory and accepts either a CA bundle file or a
hashed CA directory, covering desktop and Android.

HTTP status codes are mapped to the absl codes the retry policy keys on."
```

---

## Task 10: canonical ↔ OpenAI message mapping (pure)

Spec §4.3. The whole protocol adaptation, with no I/O. This is where the
one-to-many tool-result expansion that §3.2's `id` passthrough exists for
actually happens.

**Files:**
- Create: `agentflow/inference/openai/message_map.h`
- Create: `agentflow/inference/openai/message_map.cc`
- Create: `agentflow/inference/openai/BUILD.bazel`
- Create: `tests/unit/inference/openai/message_map_test.cc`
- Create: `tests/unit/inference/openai/BUILD.bazel`

**Interfaces:**
- Consumes: `ChatConversationOptions` (Task 2).
- Produces (all in `namespace agentflow::openai`):
  - `ToOpenAiMessages(std::string_view canonical_message_json) -> absl::StatusOr<std::vector<nlohmann::json>>` — 1 canonical message in, 1..N OpenAI messages out
  - `SystemMessage(std::string_view system_message_json) -> std::optional<nlohmann::json>`
  - `BuildRequestBody(std::string_view model, const ChatConversationOptions&, const std::vector<nlohmann::json>& messages, bool stream) -> std::string`
  - `ResponseToCanonical(std::string_view body) -> absl::StatusOr<std::string>`
  - Task 12 calls all four.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/inference/openai/message_map_test.cc`:

```cpp
// tests/unit/inference/openai/message_map_test.cc
#include "agentflow/inference/openai/message_map.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

TEST(SystemMessageTest, BareContentArrayBecomesAStringContent) {
  // ChatConversationOptions.system_message_json is a BARE array, not an object.
  auto m = SystemMessage(R"([{"type":"text","text":"You are helpful."}])");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(*m, json({{"role", "system"}, {"content", "You are helpful."}}));
}

TEST(SystemMessageTest, ConcatenatesMultipleTextItems) {
  auto m = SystemMessage(
      R"([{"type":"text","text":"a"},{"type":"text","text":"b"}])");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ((*m)["content"], "ab");
}

TEST(SystemMessageTest, EmptyOrUnparseableYieldsNothing) {
  EXPECT_FALSE(SystemMessage("").has_value());
  EXPECT_FALSE(SystemMessage("not json").has_value());
}

TEST(ToOpenAiMessagesTest, UserContentArrayFlattensToAString) {
  auto r = ToOpenAiMessages(
      R"({"role":"user","content":[{"type":"text","text":"hi"}]})");
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 1u);
  EXPECT_EQ((*r)[0], json({{"role", "user"}, {"content", "hi"}}));
}

TEST(ToOpenAiMessagesTest, AssistantWithToolCallsIsPassedThrough) {
  auto r = ToOpenAiMessages(
      R"({"role":"assistant","content":[{"type":"text","text":"let me look"}],)"
      R"("tool_calls":[{"id":"call_1","function":{"name":"s","arguments":"{}"}}]})");
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 1u);
  const json& m = (*r)[0];
  EXPECT_EQ(m["role"], "assistant");
  EXPECT_EQ(m["content"], "let me look");
  ASSERT_EQ(m["tool_calls"].size(), 1u);
  EXPECT_EQ(m["tool_calls"][0]["id"], "call_1");
  EXPECT_EQ(m["tool_calls"][0]["type"], "function");
}

TEST(ToOpenAiMessagesTest, OneToolMessageExpandsToOnePerResult) {
  // THE reason ChatConversationOptions carries tool-call ids (design spec §3.2):
  // OpenAI needs one message per result, each with its own tool_call_id.
  auto r = ToOpenAiMessages(
      R"({"role":"tool","content":[)"
      R"({"id":"call_1","name":"search","response":{"value":"A"}},)"
      R"({"id":"call_2","name":"lookup","response":{"value":"B"}}]})");
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 2u);
  EXPECT_EQ((*r)[0], json({{"role", "tool"},
                           {"tool_call_id", "call_1"},
                           {"content", "A"}}));
  EXPECT_EQ((*r)[1], json({{"role", "tool"},
                           {"tool_call_id", "call_2"},
                           {"content", "B"}}));
}

TEST(ToOpenAiMessagesTest, ToolResultWithoutAnIdIsRejected) {
  // Better a clear error than a request OpenAI rejects with an opaque 400.
  auto r = ToOpenAiMessages(
      R"({"role":"tool","content":[{"name":"search","response":{"value":"A"}}]})");
  EXPECT_FALSE(r.ok());
}

TEST(BuildRequestBodyTest, CarriesModelStreamToolsAndMessages) {
  ChatConversationOptions opts;
  opts.tools_json =
      R"([{"type":"function","function":{"name":"s","description":"d",)"
      R"("parameters":{"type":"object"}}}])";
  opts.max_output_tokens = 256;

  std::vector<json> msgs = {{{"role", "user"}, {"content", "hi"}}};
  json body = json::parse(BuildRequestBody("deepseek-chat", opts, msgs,
                                            /*stream=*/true));

  EXPECT_EQ(body["model"], "deepseek-chat");
  EXPECT_EQ(body["stream"], true);
  EXPECT_EQ(body["max_tokens"], 256);
  EXPECT_EQ(body["messages"][0]["content"], "hi");
  // BuildToolsJson already emits the OpenAI shape — passed through verbatim.
  EXPECT_EQ(body["tools"][0]["function"]["name"], "s");
}

TEST(BuildRequestBodyTest, OmitsToolsWhenThereAreNone) {
  ChatConversationOptions opts;  // tools_json defaults to "[]"
  std::vector<json> msgs = {{{"role", "user"}, {"content", "hi"}}};
  json body = json::parse(BuildRequestBody("m", opts, msgs, false));
  EXPECT_FALSE(body.contains("tools"));
  EXPECT_EQ(body["stream"], false);
}

TEST(ResponseToCanonicalTest, PlainTextAnswer) {
  auto c = ResponseToCanonical(
      R"({"choices":[{"message":{"role":"assistant","content":"42"}}]})");
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(json::parse(*c),
            json::parse(
                R"({"role":"assistant","content":[{"type":"text","text":"42"}]})"));
}

TEST(ResponseToCanonicalTest, ToolCallsArePassedThroughVerbatim) {
  auto c = ResponseToCanonical(
      R"({"choices":[{"message":{"role":"assistant","content":null,)"
      R"("tool_calls":[{"id":"call_9","type":"function",)"
      R"("function":{"name":"s","arguments":"{\"q\":1}"}}]}}]})");
  ASSERT_TRUE(c.ok());
  json got = json::parse(*c);
  ASSERT_TRUE(got.contains("tool_calls"));
  EXPECT_EQ(got["tool_calls"][0]["id"], "call_9");
  EXPECT_EQ(got["tool_calls"][0]["function"]["name"], "s");
}

TEST(ResponseToCanonicalTest, MalformedOrEmptyChoicesIsAnError) {
  EXPECT_FALSE(ResponseToCanonical("not json").ok());
  EXPECT_FALSE(ResponseToCanonical(R"({"choices":[]})").ok());
}

}  // namespace
}  // namespace agentflow::openai
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test //tests/unit/inference/openai:message_map_test`
Expected: FAIL — `agentflow/inference/openai/message_map.h` does not exist.

- [ ] **Step 3: Write the header**

Create `agentflow/inference/openai/message_map.h`:

```cpp
// agentflow/inference/openai/message_map.h
//
// Pure conversion between agentflow's canonical message shape (which is
// LiteRT-LM's) and the OpenAI /v1/chat/completions shape. No I/O.
#ifndef AGENTFLOW_INFERENCE_OPENAI_MESSAGE_MAP_H_
#define AGENTFLOW_INFERENCE_OPENAI_MESSAGE_MAP_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

#include "agentflow/inference/chat_backend.h"

namespace agentflow::openai {

// ChatConversationOptions.system_message_json is a BARE content array
// ([{"type":"text","text":"..."}]), not a {role,content} object — LiteRT-LM
// wraps it itself. Returns nullopt when empty or unparseable.
std::optional<nlohmann::json> SystemMessage(
    std::string_view system_message_json);

// Converts ONE canonical message into 1..N OpenAI messages.
//
// The one-to-many case is `role:"tool"`: a single canonical tool message can
// carry several results, and OpenAI requires each to be its own message with
// its own tool_call_id. A result entry missing `id` is an InvalidArgumentError
// rather than a request the server will reject opaquely.
absl::StatusOr<std::vector<nlohmann::json>> ToOpenAiMessages(
    std::string_view canonical_message_json);

// Builds the request body. `opts.tools_json` is already the OpenAI tools shape
// (AgentNode::BuildToolsJson emits it), so it is passed through verbatim; an
// empty array is omitted entirely.
std::string BuildRequestBody(std::string_view model,
                              const ChatConversationOptions& opts,
                              const std::vector<nlohmann::json>& messages,
                              bool stream);

// Converts a NON-streaming /v1/chat/completions response body into canonical
// assistant JSON. (The streaming path uses StreamAccumulator instead.)
absl::StatusOr<std::string> ResponseToCanonical(std::string_view body);

}  // namespace agentflow::openai
#endif  // AGENTFLOW_INFERENCE_OPENAI_MESSAGE_MAP_H_
```

- [ ] **Step 4: Write the implementation**

Create `agentflow/inference/openai/message_map.cc`:

```cpp
// agentflow/inference/openai/message_map.cc
#include "agentflow/inference/openai/message_map.h"

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

// Flattens a canonical content array into a single string. Canonical content
// is [{"type":"text","text":"..."}]; OpenAI wants a plain string.
std::string FlattenContent(const json& content) {
  if (content.is_string()) return content.get<std::string>();
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

}  // namespace

std::optional<nlohmann::json> SystemMessage(
    std::string_view system_message_json) {
  if (system_message_json.empty()) return std::nullopt;
  json arr = json::parse(system_message_json, nullptr, false);
  if (arr.is_discarded()) return std::nullopt;
  std::string text = FlattenContent(arr);
  if (text.empty()) return std::nullopt;
  return json{{"role", "system"}, {"content", std::move(text)}};
}

absl::StatusOr<std::vector<nlohmann::json>> ToOpenAiMessages(
    std::string_view canonical_message_json) {
  json m = json::parse(canonical_message_json, nullptr, false);
  if (m.is_discarded() || !m.is_object()) {
    return absl::InvalidArgumentError("canonical message is not a JSON object");
  }
  const std::string role = m.value("role", "");
  std::vector<json> out;

  if (role == "tool") {
    if (!m.contains("content") || !m["content"].is_array()) {
      return absl::InvalidArgumentError("tool message has no content array");
    }
    for (const auto& entry : m["content"]) {
      const std::string id = entry.value("id", "");
      if (id.empty()) {
        return absl::InvalidArgumentError(absl::StrCat(
            "tool result for '", entry.value("name", "?"),
            "' has no id; OpenAI requires tool_call_id on every tool message"));
      }
      std::string value;
      if (entry.contains("response") && entry["response"].contains("value")) {
        const auto& v = entry["response"]["value"];
        value = v.is_string() ? v.get<std::string>() : v.dump();
      }
      out.push_back({{"role", "tool"},
                     {"tool_call_id", id},
                     {"content", std::move(value)}});
    }
    return out;
  }

  json msg = {{"role", role.empty() ? "user" : role}};
  msg["content"] = m.contains("content") ? FlattenContent(m["content"])
                                          : std::string{};
  if (m.contains("tool_calls") && m["tool_calls"].is_array() &&
      !m["tool_calls"].empty()) {
    json calls = json::array();
    for (const auto& tc : m["tool_calls"]) {
      json call = tc;
      // OpenAI requires an explicit type discriminator; LiteRT-LM omits it.
      if (!call.contains("type")) call["type"] = "function";
      calls.push_back(std::move(call));
    }
    msg["tool_calls"] = std::move(calls);
  }
  out.push_back(std::move(msg));
  return out;
}

std::string BuildRequestBody(std::string_view model,
                              const ChatConversationOptions& opts,
                              const std::vector<nlohmann::json>& messages,
                              bool stream) {
  json body;
  body["model"] = std::string(model);
  body["messages"] = messages;
  body["stream"] = stream;
  if (opts.max_output_tokens > 0) body["max_tokens"] = opts.max_output_tokens;

  json tools = json::parse(opts.tools_json, nullptr, false);
  if (!tools.is_discarded() && tools.is_array() && !tools.empty()) {
    body["tools"] = std::move(tools);
  }
  return body.dump();
}

absl::StatusOr<std::string> ResponseToCanonical(std::string_view body) {
  json resp = json::parse(body, nullptr, false);
  if (resp.is_discarded()) {
    return absl::InternalError("OpenAI response is not valid JSON");
  }
  if (!resp.contains("choices") || !resp["choices"].is_array() ||
      resp["choices"].empty()) {
    return absl::InternalError("OpenAI response has no choices");
  }
  const json& msg = resp["choices"][0]["message"];

  json out = {{"role", "assistant"}};
  std::string text;
  if (msg.contains("content") && msg["content"].is_string()) {
    text = msg["content"].get<std::string>();
  }
  out["content"] = json::array({{{"type", "text"}, {"text", text}}});

  if (msg.contains("tool_calls") && msg["tool_calls"].is_array() &&
      !msg["tool_calls"].empty()) {
    out["tool_calls"] = msg["tool_calls"];
  }
  return out.dump();
}

}  // namespace agentflow::openai
```

- [ ] **Step 5: Add the Bazel targets**

Create `agentflow/inference/openai/BUILD.bazel`:

```python
# agentflow/inference/openai/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "message_map",
    srcs = ["message_map.cc"],
    hdrs = ["message_map.h"],
    deps = [
        "//agentflow/inference:chat_backend",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@nlohmann_json//:json",
    ],
)
```

Create `tests/unit/inference/openai/BUILD.bazel`:

```python
# tests/unit/inference/openai/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "message_map_test",
    size = "small",
    srcs = ["message_map_test.cc"],
    deps = [
        "//agentflow/inference/openai:message_map",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test //tests/unit/inference/openai:message_map_test --test_output=all`
Expected: PASS — 12 tests.

- [ ] **Step 7: Commit**

```bash
git add agentflow/inference/openai/ tests/unit/inference/openai/
git commit -m "feat(openai): canonical <-> OpenAI message mapping

Pure, socket-free protocol adaptation. The tool-result path expands one
canonical message into N OpenAI messages, each carrying the tool_call_id that
AgentNode threads through the canonical shape; a result without an id is a
clear error rather than an opaque server-side 400."
```

---

## Task 11: OpenAI streaming accumulator (pure)

Spec §4.4, §7.2. The single most error-prone piece of OpenAI streaming:
tool-call arguments arrive fragmented across frames and must be rejoined by
index.

**Files:**
- Create: `agentflow/inference/openai/stream_accumulator.h`
- Create: `agentflow/inference/openai/stream_accumulator.cc`
- Create: `tests/unit/inference/openai/stream_accumulator_test.cc`
- Modify: `agentflow/inference/openai/BUILD.bazel`
- Modify: `tests/unit/inference/openai/BUILD.bazel`

**Interfaces:**
- Consumes: nothing (pure JSON).
- Produces: `agentflow::openai::StreamAccumulator` with `std::string Feed(std::string_view frame_json)` (returns this frame's text delta, `""` if none) and `std::string Canonical() const`. Task 12 drives it from the `SseHandler`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/inference/openai/stream_accumulator_test.cc`:

```cpp
// tests/unit/inference/openai/stream_accumulator_test.cc
#include "agentflow/inference/openai/stream_accumulator.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

std::string TextFrame(const std::string& piece) {
  json f = {{"choices", json::array({{{"delta", {{"content", piece}}}}})}};
  return f.dump();
}

TEST(StreamAccumulatorTest, JoinsTextDeltasAndReportsEachOne) {
  StreamAccumulator a;
  EXPECT_EQ(a.Feed(TextFrame("He")), "He");
  EXPECT_EQ(a.Feed(TextFrame("llo")), "llo");

  EXPECT_EQ(json::parse(a.Canonical()),
            json::parse(
                R"({"role":"assistant","content":[{"type":"text","text":"Hello"}]})"));
}

TEST(StreamAccumulatorTest, RejoinsToolCallArgumentsSplitAcrossFrames) {
  // id and function.name appear ONLY in the first frame for an index; later
  // frames carry bare argument fragments.
  StreamAccumulator a;
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
         R"("function":{"name":"search","arguments":"{\"q\":"}}]}}]})");
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
         R"("function":{"arguments":"\"zen\"}"}}]}}]})");

  json got = json::parse(a.Canonical());
  ASSERT_TRUE(got.contains("tool_calls"));
  ASSERT_EQ(got["tool_calls"].size(), 1u);
  EXPECT_EQ(got["tool_calls"][0]["id"], "call_1");
  EXPECT_EQ(got["tool_calls"][0]["function"]["name"], "search");
  // Arguments stay a STRING, as OpenAI sends and AgentNode expects.
  EXPECT_EQ(got["tool_calls"][0]["function"]["arguments"], R"({"q":"zen"})");
}

TEST(StreamAccumulatorTest, MergesParallelToolCallsByIndex) {
  StreamAccumulator a;
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[)"
         R"({"index":0,"id":"c0","function":{"name":"a","arguments":"{}"}},)"
         R"({"index":1,"id":"c1","function":{"name":"b","arguments":"{"}}]}}]})");
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[)"
         R"({"index":1,"function":{"arguments":"}"}}]}}]})");

  json got = json::parse(a.Canonical());
  ASSERT_EQ(got["tool_calls"].size(), 2u);
  EXPECT_EQ(got["tool_calls"][0]["id"], "c0");
  EXPECT_EQ(got["tool_calls"][1]["id"], "c1");
  EXPECT_EQ(got["tool_calls"][1]["function"]["arguments"], "{}");
}

TEST(StreamAccumulatorTest, HandlesTextAndToolCallsInOneStream) {
  StreamAccumulator a;
  EXPECT_EQ(a.Feed(TextFrame("thinking")), "thinking");
  a.Feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c",)"
         R"("function":{"name":"n","arguments":"{}"}}]}}]})");

  json got = json::parse(a.Canonical());
  EXPECT_EQ(got["content"][0]["text"], "thinking");
  EXPECT_EQ(got["tool_calls"][0]["id"], "c");
}

TEST(StreamAccumulatorTest, IgnoresRoleOnlyAndEmptyDeltaFrames) {
  StreamAccumulator a;
  EXPECT_EQ(a.Feed(R"({"choices":[{"delta":{"role":"assistant"}}]})"), "");
  EXPECT_EQ(a.Feed(R"({"choices":[{"delta":{}}]})"), "");
  EXPECT_EQ(a.Feed(R"({"choices":[{"delta":{"content":null}}]})"), "");
  EXPECT_EQ(json::parse(a.Canonical())["content"][0]["text"], "");
}

TEST(StreamAccumulatorTest, IgnoresMalformedFramesRatherThanThrowing) {
  // A provider emitting a stray keep-alive or truncated frame must not abort
  // a half-finished answer.
  StreamAccumulator a;
  EXPECT_EQ(a.Feed("not json"), "");
  EXPECT_EQ(a.Feed(R"({"no_choices":true})"), "");
  EXPECT_EQ(a.Feed(TextFrame("ok")), "ok");
  EXPECT_EQ(json::parse(a.Canonical())["content"][0]["text"], "ok");
}

}  // namespace
}  // namespace agentflow::openai
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test //tests/unit/inference/openai:stream_accumulator_test`
Expected: FAIL — `stream_accumulator.h` does not exist.

- [ ] **Step 3: Write the header**

Create `agentflow/inference/openai/stream_accumulator.h`:

```cpp
// agentflow/inference/openai/stream_accumulator.h
#ifndef AGENTFLOW_INFERENCE_OPENAI_STREAM_ACCUMULATOR_H_
#define AGENTFLOW_INFERENCE_OPENAI_STREAM_ACCUMULATOR_H_

#include <map>
#include <string>
#include <string_view>

namespace agentflow::openai {

// Accumulates OpenAI streaming frames into one canonical assistant message.
//
// Tool-call arguments arrive fragmented: for a given `index`, the first frame
// carries `id` and `function.name`, and every later frame carries only another
// piece of `function.arguments` to concatenate. Getting this wrong produces
// truncated or interleaved JSON arguments, so it is covered exhaustively by
// stream_accumulator_test.
//
// Malformed frames are ignored rather than treated as errors — a stray
// keep-alive must not abort a half-finished answer.
class StreamAccumulator {
 public:
  // Feeds one SSE data payload (already stripped; never "[DONE]").
  // Returns the text delta this frame contained, or "" if it carried none.
  std::string Feed(std::string_view frame_json);

  // The canonical assistant JSON for everything fed so far.
  std::string Canonical() const;

 private:
  struct PartialCall {
    std::string id;
    std::string name;
    std::string arguments;
  };

  std::string text_;
  // Keyed by the stream's `index` so parallel calls stay separate and ordered.
  std::map<int, PartialCall> calls_;
};

}  // namespace agentflow::openai
#endif  // AGENTFLOW_INFERENCE_OPENAI_STREAM_ACCUMULATOR_H_
```

- [ ] **Step 4: Write the implementation**

Create `agentflow/inference/openai/stream_accumulator.cc`:

```cpp
// agentflow/inference/openai/stream_accumulator.cc
#include "agentflow/inference/openai/stream_accumulator.h"

#include <nlohmann/json.hpp>

namespace agentflow::openai {
namespace {
using json = nlohmann::json;
}  // namespace

std::string StreamAccumulator::Feed(std::string_view frame_json) {
  json f = json::parse(frame_json, nullptr, /*allow_exceptions=*/false);
  if (f.is_discarded()) return {};
  if (!f.contains("choices") || !f["choices"].is_array() ||
      f["choices"].empty()) {
    return {};
  }
  const json& choice = f["choices"][0];
  if (!choice.contains("delta") || !choice["delta"].is_object()) return {};
  const json& delta = choice["delta"];

  std::string text_delta;
  if (delta.contains("content") && delta["content"].is_string()) {
    text_delta = delta["content"].get<std::string>();
    text_.append(text_delta);
  }

  if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
    for (const auto& tc : delta["tool_calls"]) {
      const int index = tc.value("index", 0);
      PartialCall& call = calls_[index];
      // id and name appear only in this index's FIRST frame; never overwrite
      // them with a later frame's absent value.
      if (tc.contains("id") && tc["id"].is_string()) {
        call.id = tc["id"].get<std::string>();
      }
      if (tc.contains("function") && tc["function"].is_object()) {
        const json& fn = tc["function"];
        if (fn.contains("name") && fn["name"].is_string()) {
          call.name = fn["name"].get<std::string>();
        }
        if (fn.contains("arguments") && fn["arguments"].is_string()) {
          // Fragment — append, never replace.
          call.arguments.append(fn["arguments"].get<std::string>());
        }
      }
    }
  }
  return text_delta;
}

std::string StreamAccumulator::Canonical() const {
  json out = {{"role", "assistant"}};
  out["content"] = json::array({{{"type", "text"}, {"text", text_}}});

  if (!calls_.empty()) {
    json arr = json::array();
    for (const auto& [index, call] : calls_) {  // std::map → ordered by index
      arr.push_back({{"id", call.id},
                     {"type", "function"},
                     {"function",
                      {{"name", call.name}, {"arguments", call.arguments}}}});
    }
    out["tool_calls"] = std::move(arr);
  }
  return out.dump();
}

}  // namespace agentflow::openai
```

- [ ] **Step 5: Add the Bazel targets**

Append to `agentflow/inference/openai/BUILD.bazel`:

```python
cc_library(
    name = "stream_accumulator",
    srcs = ["stream_accumulator.cc"],
    hdrs = ["stream_accumulator.h"],
    deps = ["@nlohmann_json//:json"],
)
```

Append to `tests/unit/inference/openai/BUILD.bazel`:

```python
cc_test(
    name = "stream_accumulator_test",
    size = "small",
    srcs = ["stream_accumulator_test.cc"],
    deps = [
        "//agentflow/inference/openai:stream_accumulator",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test //tests/unit/inference/openai:stream_accumulator_test --test_output=all`
Expected: PASS — 6 tests.

- [ ] **Step 7: Commit**

```bash
git add agentflow/inference/openai/ tests/unit/inference/openai/
git commit -m "feat(openai): streaming delta accumulator

Rejoins tool-call arguments fragmented across SSE frames, keyed by index so
parallel calls stay separate. Malformed frames are ignored rather than
aborting a half-finished answer."
```

---

## Task 12: `OpenAiChatBackend` — wiring, retry, cancellation

Spec §3.3, §6, §7.2. Assembles the pieces into an `IChatBackend`. This is where
the retry constraint that protects the UI lives.

**Files:**
- Create: `agentflow/inference/openai/openai_chat_backend.h`
- Create: `agentflow/inference/openai/openai_chat_backend.cc`
- Create: `tests/support/fake_http_client.h`
- Create: `tests/unit/inference/openai/openai_chat_backend_test.cc`
- Modify: `agentflow/inference/openai/BUILD.bazel`
- Modify: `tests/support/BUILD.bazel`
- Modify: `tests/unit/inference/openai/BUILD.bazel`

**Interfaces:**
- Consumes: `IChatBackend`, `IConversation` (Task 2); `IHttpClient`, `HttpRequest` (Task 8); `SystemMessage`, `ToOpenAiMessages`, `BuildRequestBody`, `ResponseToCanonical` (Task 10); `StreamAccumulator` (Task 11).
- Produces:
  - `agentflow::openai::OpenAiOptions{base_url, api_key, model, max_retries, retry_base_delay}`
  - `agentflow::openai::OpenAiChatBackend::Create(OpenAiOptions, net::IHttpClient&) -> std::shared_ptr<OpenAiChatBackend>` — note the **concrete** return type, not `shared_ptr<IChatBackend>`: callers need `last_warning()`, which is not on the interface. It converts implicitly wherever a `shared_ptr<IChatBackend>` is wanted (e.g. `spec.backends["cloud"]`).
  - `OpenAiChatBackend::last_warning() -> const std::string&`
  - Task 13's example constructs one.

- [ ] **Step 1: Write the fake HTTP client**

Create `tests/support/fake_http_client.h`:

```cpp
// tests/support/fake_http_client.h
//
// Replays canned SSE frames and canned non-streaming bodies so the OpenAI
// backend's request building, mapping and retry policy can be tested with no
// network.
#ifndef TESTS_SUPPORT_FAKE_HTTP_CLIENT_H_
#define TESTS_SUPPORT_FAKE_HTTP_CLIENT_H_

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "agentflow/net/http_client.h"

namespace agentflow::testing {

// One scripted attempt: either a status to fail with, or frames to deliver
// (optionally emitting some frames BEFORE failing, to exercise the
// "no retry after a token was emitted" rule).
struct FakeHttpTurn {
  std::vector<std::string> frames;   // SSE data payloads
  absl::Status status = absl::OkStatus();
  std::string body;                  // for Post()
};

class FakeHttpClient : public net::IHttpClient {
 public:
  explicit FakeHttpClient(std::vector<FakeHttpTurn> turns)
      : turns_(turns.begin(), turns.end()) {}

  asio::awaitable<absl::Status> PostSse(net::HttpRequest req,
                                         const net::SseHandler& on_event,
                                         const CancelToken& cancel) override {
    requests_.push_back(req);
    if (turns_.empty()) co_return absl::UnavailableError("fake: no turns left");
    FakeHttpTurn turn = std::move(turns_.front());
    turns_.pop_front();
    for (const auto& f : turn.frames) {
      if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");
      if (on_event) co_await on_event(f);
    }
    co_return turn.status;
  }

  asio::awaitable<absl::StatusOr<std::string>> Post(
      net::HttpRequest req, const CancelToken&) override {
    requests_.push_back(req);
    if (turns_.empty()) co_return absl::UnavailableError("fake: no turns left");
    FakeHttpTurn turn = std::move(turns_.front());
    turns_.pop_front();
    if (!turn.status.ok()) co_return turn.status;
    co_return turn.body;
  }

  // Every request received, in order. Lets a test assert the request body and
  // that credentials went into a header rather than the body.
  const std::vector<net::HttpRequest>& requests() const { return requests_; }
  int attempts() const { return static_cast<int>(requests_.size()); }

 private:
  std::deque<FakeHttpTurn> turns_;
  std::vector<net::HttpRequest> requests_;
};

}  // namespace agentflow::testing
#endif  // TESTS_SUPPORT_FAKE_HTTP_CLIENT_H_
```

Append to `tests/support/BUILD.bazel`:

```python
cc_library(
    name = "fake_http_client",
    testonly = True,
    hdrs = ["fake_http_client.h"],
    deps = [
        "//agentflow/core",
        "//agentflow/net:http_client",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@asio",
    ],
)
```

- [ ] **Step 2: Write the failing test**

Create `tests/unit/inference/openai/openai_chat_backend_test.cc`:

```cpp
// tests/unit/inference/openai/openai_chat_backend_test.cc
#include "agentflow/inference/openai/openai_chat_backend.h"

#include <memory>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "tests/support/fake_http_client.h"

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

OpenAiOptions TestOptions() {
  OpenAiOptions o;
  o.base_url = "https://api.example.com/v1";
  o.api_key = "sk-secret";
  o.model = "test-model";
  o.max_retries = 3;
  o.retry_base_delay = std::chrono::milliseconds(1);  // keep tests fast
  return o;
}

std::string TextFrame(const std::string& piece) {
  json f = {{"choices", json::array({{{"delta", {{"content", piece}}}}})}};
  return f.dump();
}

struct SendResult {
  absl::StatusOr<std::string> response;
  std::vector<std::string> deltas;
};

SendResult Send(IConversation& conv, const std::string& message_json,
                 asio::io_context& io, const CancelToken& cancel) {
  SendResult r;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await conv.SendAsync(
            message_json,
            [&](std::string_view d) -> asio::awaitable<void> {
              r.deltas.emplace_back(d);
              co_return;
            },
            cancel);
      },
      asio::use_future);
  io.run();
  io.restart();
  r.response = fut.get();
  return r;
}

TEST(OpenAiChatBackendTest, DescribeNamesTheModelAndHidesTheKey) {
  asio::io_context io;
  testing::FakeHttpClient http({});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  EXPECT_EQ(backend->Describe(), "openai:test-model");
  EXPECT_EQ(std::string(backend->Describe()).find("sk-secret"),
            std::string::npos);
}

TEST(OpenAiChatBackendTest, StreamsDeltasAndReturnsCanonicalJson) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("He"), TextFrame("llo")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"hi"}]})",
                 io, cancel.Token());

  ASSERT_TRUE(r.response.ok()) << r.response.status().message();
  EXPECT_EQ(r.deltas, (std::vector<std::string>{"He", "llo"}));
  EXPECT_EQ(json::parse(*r.response)["content"][0]["text"], "Hello");
}

TEST(OpenAiChatBackendTest, ApiKeyTravelsInTheHeaderNeverTheBody) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("ok")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  Send(*conv, R"({"role":"user","content":[{"type":"text","text":"hi"}]})", io,
       cancel.Token());

  ASSERT_EQ(http.requests().size(), 1u);
  const auto& req = http.requests()[0];
  EXPECT_EQ(req.url, "https://api.example.com/v1/chat/completions");
  EXPECT_EQ(req.body.find("sk-secret"), std::string::npos);
  bool found = false;
  for (const auto& [k, v] : req.headers) {
    if (k == "Authorization") {
      EXPECT_EQ(v, "Bearer sk-secret");
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(OpenAiChatBackendTest, HistoryIsOwnedSoTurnTwoCarriesTurnOne) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("first")}},
                                {.frames = {TextFrame("second")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  Send(*conv, R"({"role":"user","content":[{"type":"text","text":"one"}]})", io,
       cancel.Token());
  Send(*conv, R"({"role":"user","content":[{"type":"text","text":"two"}]})", io,
       cancel.Token());

  ASSERT_EQ(http.requests().size(), 2u);
  json body2 = json::parse(http.requests()[1].body);
  // user "one", assistant "first", user "two"
  ASSERT_EQ(body2["messages"].size(), 3u);
  EXPECT_EQ(body2["messages"][0]["content"], "one");
  EXPECT_EQ(body2["messages"][1]["role"], "assistant");
  EXPECT_EQ(body2["messages"][1]["content"], "first");
  EXPECT_EQ(body2["messages"][2]["content"], "two");
}

TEST(OpenAiChatBackendTest, RetriesUnavailableBeforeAnyTokenIsEmitted) {
  asio::io_context io;
  testing::FakeHttpClient http({
      {.status = absl::UnavailableError("503")},
      {.status = absl::UnavailableError("503")},
      {.frames = {TextFrame("ok")}},
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());

  ASSERT_TRUE(r.response.ok());
  EXPECT_EQ(http.attempts(), 3);
  EXPECT_EQ(r.deltas, (std::vector<std::string>{"ok"}));
}

TEST(OpenAiChatBackendTest, DoesNotRetryOnceATokenHasBeenEmitted) {
  // THE UI-protecting rule (design spec §6): retrying after the user has
  // already seen partial output would duplicate it on screen.
  asio::io_context io;
  testing::FakeHttpClient http({
      {.frames = {TextFrame("par")}, .status = absl::UnavailableError("dropped")},
      {.frames = {TextFrame("whole answer")}},  // must never be reached
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());

  EXPECT_FALSE(r.response.ok());
  EXPECT_EQ(http.attempts(), 1);
  EXPECT_EQ(r.deltas, (std::vector<std::string>{"par"}));
}

TEST(OpenAiChatBackendTest, DoesNotRetryClientErrors) {
  asio::io_context io;
  testing::FakeHttpClient http({
      {.status = absl::PermissionDeniedError("401 bad key")},
      {.frames = {TextFrame("never")}},
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());

  EXPECT_FALSE(r.response.ok());
  EXPECT_EQ(r.response.status().code(), absl::StatusCode::kPermissionDenied);
  EXPECT_EQ(http.attempts(), 1);
}

TEST(OpenAiChatBackendTest, GivesUpAfterMaxRetries) {
  asio::io_context io;
  testing::FakeHttpClient http({
      {.status = absl::UnavailableError("1")},
      {.status = absl::UnavailableError("2")},
      {.status = absl::UnavailableError("3")},
  });
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());

  EXPECT_FALSE(r.response.ok());
  EXPECT_EQ(http.attempts(), 3);
}

TEST(OpenAiChatBackendTest, ToolResultMessageBecomesOneOpenAiMessagePerResult) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("done")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);
  auto conv = backend->CreateConversation(ChatConversationOptions{});

  CancelSource cancel;
  Send(*conv,
       R"({"role":"tool","content":[)"
       R"({"id":"c1","name":"a","response":{"value":"A"}},)"
       R"({"id":"c2","name":"b","response":{"value":"B"}}]})",
       io, cancel.Token());

  json body = json::parse(http.requests()[0].body);
  ASSERT_EQ(body["messages"].size(), 2u);
  EXPECT_EQ(body["messages"][0]["tool_call_id"], "c1");
  EXPECT_EQ(body["messages"][1]["tool_call_id"], "c2");
}

TEST(OpenAiChatBackendTest, ConstrainedToolCallsIsReportedNotSilentlyDropped) {
  asio::io_context io;
  testing::FakeHttpClient http({{.frames = {TextFrame("ok")}}});
  auto backend = OpenAiChatBackend::Create(TestOptions(), http);

  ChatConversationOptions opts;
  opts.constrained_tool_calls = true;
  auto conv = backend->CreateConversation(std::move(opts));
  ASSERT_NE(conv, nullptr);  // still usable — it runs unconstrained

  CancelSource cancel;
  auto r = Send(*conv, R"({"role":"user","content":[{"type":"text","text":"x"}]})",
                 io, cancel.Token());
  EXPECT_TRUE(r.response.ok());
  EXPECT_TRUE(backend->last_warning().find("constrained") != std::string::npos);
}

}  // namespace
}  // namespace agentflow::openai
```

- [ ] **Step 3: Run it to verify it fails**

Run: `bazel test //tests/unit/inference/openai:openai_chat_backend_test`
Expected: FAIL — `openai_chat_backend.h` does not exist.

- [ ] **Step 4: Write the header**

Create `agentflow/inference/openai/openai_chat_backend.h`:

```cpp
// agentflow/inference/openai/openai_chat_backend.h
#ifndef AGENTFLOW_INFERENCE_OPENAI_OPENAI_CHAT_BACKEND_H_
#define AGENTFLOW_INFERENCE_OPENAI_OPENAI_CHAT_BACKEND_H_

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "agentflow/inference/chat_backend.h"
#include "agentflow/net/http_client.h"

namespace agentflow::openai {

struct OpenAiOptions {
  // Without a trailing slash; "/chat/completions" is appended.
  // e.g. "https://api.deepseek.com/v1", "http://127.0.0.1:11434/v1".
  std::string base_url;
  // Sent as "Authorization: Bearer <key>". NEVER placed in the request body,
  // in Describe(), or in any error message.
  std::string api_key;
  std::string model;

  // Total attempts, not retries-after-the-first. 1 disables retrying.
  int max_retries = 3;
  std::chrono::milliseconds retry_base_delay{100};
};

// IChatBackend over an OpenAI-compatible /v1/chat/completions endpoint.
//
// Covers OpenAI, DeepSeek, Volcengine ARK, Kimi, GLM, MiniMax, OpenRouter,
// Ollama, vLLM and LiteLLM gateways — they differ only in base_url, api_key
// and model.
//
// HTTP is stateless, so each conversation owns its own messages array and
// resends the history every turn. (The on-device backend does the opposite:
// the engine owns history so the KV cache is reused. Both satisfy the same
// IConversation contract.)
class OpenAiChatBackend : public IChatBackend {
 public:
  // `http` must outlive the backend and every conversation it creates.
  static std::shared_ptr<OpenAiChatBackend> Create(OpenAiOptions opts,
                                                    net::IHttpClient& http);

  std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) override;

  // "openai:<model>". Never contains the key.
  std::string_view Describe() const override { return describe_; }

  // The most recent capability warning, e.g. that constrained_tool_calls was
  // requested but cannot be honoured. Empty when none. Hosts surface this;
  // the backend never silently drops a correctness guarantee.
  const std::string& last_warning() const { return last_warning_; }

 private:
  OpenAiChatBackend(OpenAiOptions opts, net::IHttpClient& http);

  OpenAiOptions opts_;
  net::IHttpClient& http_;
  std::string describe_;
  std::string last_warning_;
};

}  // namespace agentflow::openai
#endif  // AGENTFLOW_INFERENCE_OPENAI_OPENAI_CHAT_BACKEND_H_
```

- [ ] **Step 5: Write the implementation**

Create `agentflow/inference/openai/openai_chat_backend.cc`:

```cpp
// agentflow/inference/openai/openai_chat_backend.cc
#include "agentflow/inference/openai/openai_chat_backend.h"

#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

#include "agentflow/inference/openai/message_map.h"
#include "agentflow/inference/openai/stream_accumulator.h"

namespace agentflow::openai {
namespace {

using json = nlohmann::json;

// Only transport-level and server-side failures are worth another attempt.
// A 4xx will fail identically every time.
bool IsRetryable(const absl::Status& s) {
  return s.code() == absl::StatusCode::kUnavailable ||
         s.code() == absl::StatusCode::kResourceExhausted;
}

class OpenAiConversation : public IConversation {
 public:
  OpenAiConversation(OpenAiOptions opts, net::IHttpClient& http,
                      ChatConversationOptions conv_opts)
      : opts_(std::move(opts)),
        http_(http),
        conv_opts_(std::move(conv_opts)) {
    if (auto sys = SystemMessage(conv_opts_.system_message_json)) {
      messages_.push_back(*std::move(sys));
    }
  }

  asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) override {
    auto incoming = ToOpenAiMessages(message_json);
    if (!incoming.ok()) co_return incoming.status();
    for (auto& m : *incoming) messages_.push_back(std::move(m));

    net::HttpRequest req;
    req.url = absl::StrCat(opts_.base_url, "/chat/completions");
    req.headers = {{"Content-Type", "application/json"},
                   {"Authorization", absl::StrCat("Bearer ", opts_.api_key)}};
    req.body = BuildRequestBody(opts_.model, conv_opts_, messages_,
                                 /*stream=*/true);

    absl::Status last = absl::UnknownError("no attempt made");
    for (int attempt = 0; attempt < opts_.max_retries; ++attempt) {
      if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");

      StreamAccumulator acc;
      bool emitted = false;
      auto status = co_await http_.PostSse(
          req,
          [&](std::string_view frame) -> asio::awaitable<void> {
            std::string delta = acc.Feed(frame);
            if (delta.empty()) co_return;
            emitted = true;
            // co_await, so a slow consumer back-pressures the socket read.
            if (on_token) co_await on_token(delta);
            co_return;
          },
          cancel);

      if (status.ok()) {
        std::string canonical = acc.Canonical();
        // Record the assistant turn so the next Send resends full history —
        // HTTP is stateless, unlike the on-device engine.
        auto assistant = ToOpenAiMessages(canonical);
        if (assistant.ok()) {
          for (auto& m : *assistant) messages_.push_back(std::move(m));
        }
        co_return canonical;
      }

      last = status;
      if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");

      // The UI-protecting rule (design spec §6): once the user has seen part
      // of an answer, retrying would duplicate it on screen. Report instead.
      if (emitted) {
        co_return absl::Status(
            status.code(),
            absl::StrCat("stream interrupted after partial output: ",
                          status.message()));
      }
      if (!IsRetryable(status)) co_return status;
      if (attempt + 1 >= opts_.max_retries) break;

      // Exponential backoff: base, 2×base, 4×base…
      asio::steady_timer timer(co_await asio::this_coro::executor);
      timer.expires_after(opts_.retry_base_delay * (1 << attempt));
      auto [ec] = co_await timer.async_wait(
          asio::as_tuple(asio::use_awaitable));
      (void)ec;
    }
    co_return last;
  }

  void Cancel() override {
    // The in-flight HTTP request is broken by the CancelToken hook the client
    // registered; nothing extra is owned here.
  }

 private:
  OpenAiOptions opts_;
  net::IHttpClient& http_;
  ChatConversationOptions conv_opts_;
  std::vector<json> messages_;
};

}  // namespace

OpenAiChatBackend::OpenAiChatBackend(OpenAiOptions opts, net::IHttpClient& http)
    : opts_(std::move(opts)),
      http_(http),
      describe_(absl::StrCat("openai:", opts_.model)) {}

std::shared_ptr<OpenAiChatBackend> OpenAiChatBackend::Create(
    OpenAiOptions opts, net::IHttpClient& http) {
  return std::shared_ptr<OpenAiChatBackend>(
      new OpenAiChatBackend(std::move(opts), http));
}

std::shared_ptr<IConversation> OpenAiChatBackend::CreateConversation(
    ChatConversationOptions opts) {
  if (opts.constrained_tool_calls) {
    // Do not degrade silently: an OpenAI-compatible endpoint has no equivalent
    // of LLGuidance grammar constraints, so the caller must be able to see
    // that the guarantee was dropped (design spec §6).
    last_warning_ = absl::StrCat(
        "backend ", describe_,
        " does not support constrained tool calls; running unconstrained");
  }
  return std::make_shared<OpenAiConversation>(opts_, http_, std::move(opts));
}

}  // namespace agentflow::openai
```

Add `#include <asio/as_tuple.hpp>` and `#include <asio/this_coro.hpp>` if the
compiler asks for them.

- [ ] **Step 6: Add the Bazel targets**

Append to `agentflow/inference/openai/BUILD.bazel`:

```python
cc_library(
    name = "openai_chat_backend",
    srcs = ["openai_chat_backend.cc"],
    hdrs = ["openai_chat_backend.h"],
    deps = [
        ":message_map",
        ":stream_accumulator",
        "//agentflow/inference:chat_backend",
        "//agentflow/net:http_client",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@asio",
        "@nlohmann_json//:json",
    ],
)
```

Append to `tests/unit/inference/openai/BUILD.bazel`:

```python
cc_test(
    name = "openai_chat_backend_test",
    size = "small",
    srcs = ["openai_chat_backend_test.cc"],
    deps = [
        "//agentflow/core",
        "//agentflow/inference/openai:openai_chat_backend",
        "//tests/support:fake_http_client",
        "@asio",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `bazel test //tests/unit/inference/openai:openai_chat_backend_test --test_output=all`
Expected: PASS — 10 tests.

- [ ] **Step 8: Run the whole suite**

Run: `bazel test //tests/unit/... //tests/integration/...`
Expected: all PASS.

- [ ] **Step 9: Commit**

```bash
git add agentflow/inference/openai/ tests/support/ tests/unit/inference/openai/
git commit -m "feat(openai): OpenAiChatBackend with retry and cancellation

An OpenAI-compatible endpoint is now a first-class IChatBackend, usable
anywhere the on-device backend is. Each conversation owns its messages array
since HTTP is stateless.

Retry is deliberately suppressed once a token has been emitted: retrying after
the user has seen partial output would duplicate it on screen. Client errors
are never retried. constrained_tool_calls is reported through last_warning()
rather than silently dropped."
```

---

## Task 13: Host wiring, example and documentation

Spec §1, §5, §9. Proves the end-to-end path and writes down how a host actually
uses it — including the credential rule, which is only enforceable if it is
documented where people look.

**Files:**
- Create: `examples/remote-llm/main.cc`
- Create: `examples/remote-llm/workflow.json`
- Create: `examples/remote-llm/BUILD.bazel`
- Create: `tests/integration/remote_llm_e2e_test.cc`
- Create: `tests/integration/BUILD.bazel` (if absent; otherwise modify)
- Modify: `README.md`

**Interfaces:**
- Consumes: everything from Tasks 2-12.
- Produces: no new library API — a runnable example and the documented host recipe.

- [ ] **Step 1: Write the example workflow**

Create `examples/remote-llm/workflow.json`:

```json
{
  "name": "remote-llm-demo",
  "version": "1",
  "state": {
    "fields": {
      "user_query": "string",
      "assistant_reply": "string"
    }
  },
  "agents": {
    "assistant": {
      "system_prompt": "You are a concise assistant.",
      "model": {"backend": "cloud", "max_output_tokens": 512}
    }
  },
  "graph": {
    "nodes": [{"id": "assistant", "agent": "assistant"}],
    "edges": []
  }
}
```

Note `"backend": "cloud"` — a logical name only. No URL, no key, no provider
model id ever appears in a workflow file.

- [ ] **Step 2: Write the example host**

Create `examples/remote-llm/main.cc`:

```cpp
// examples/remote-llm/main.cc
//
// Runs one agent against an OpenAI-compatible endpoint.
//
//   export AGENTFLOW_LLM_BASE_URL=https://api.deepseek.com/v1
//   export AGENTFLOW_LLM_MODEL=deepseek-chat
//   export AGENTFLOW_LLM_API_KEY=sk-...
//   bazel run //examples/remote-llm:remote_llm -- "your question"
//
// The key is read from the environment here. On Android the equivalent host
// code reads it from EncryptedSharedPreferences. Either way it is supplied by
// the HOST and never appears in the workflow JSON.
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/inference/openai/openai_chat_backend.h"
#include "agentflow/net/https_client.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "agentflow/workflow/workflow_runner.h"

namespace {

std::string RequiredEnv(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    std::cerr << "missing required environment variable: " << name << "\n";
    std::exit(2);
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string question =
      argc > 1 ? argv[1] : "Say hello in one short sentence.";

  asio::io_context io;

  // 1. Build the HTTP client and the remote backend. This is the ONLY place
  //    credentials appear.
  agentflow::net::HttpsClientOptions http_opts;
  http_opts.ca_path = "/etc/ssl/certs/ca-certificates.crt";
  agentflow::net::HttpsClient http(io, http_opts);

  agentflow::openai::OpenAiOptions llm;
  llm.base_url = RequiredEnv("AGENTFLOW_LLM_BASE_URL");
  llm.model = RequiredEnv("AGENTFLOW_LLM_MODEL");
  llm.api_key = RequiredEnv("AGENTFLOW_LLM_API_KEY");
  auto cloud = agentflow::openai::OpenAiChatBackend::Create(llm, http);

  // 2. Load the workflow and register the backend under its logical name.
  auto tools = std::make_shared<agentflow::ToolRegistry>();
  auto wf_or = agentflow::workflow::WorkflowLoader::LoadFromFile(
      "examples/remote-llm/workflow.json", *tools);
  if (!wf_or.ok()) {
    std::cerr << "failed to load workflow: " << wf_or.status().message() << "\n";
    return 1;
  }

  agentflow::workflow::AgentNodeBuildSpec spec;
  spec.workflow = *wf_or;
  spec.agent_name = "assistant";
  spec.host_tools = tools;
  spec.io_ctx = &io;
  spec.backends["cloud"] = cloud;  // matches workflow.json's model.backend

  auto built = agentflow::workflow::BuildAgentNode(spec);
  agentflow::AgentNode node(std::move(built.cfg));

  // 3. Stream the answer to stdout as it arrives.
  agentflow::CallbackEventEmitter emit(
      [](const agentflow::proto::TraceEvent& e) {
        if (e.has_token()) {
          std::cout << e.token().token() << std::flush;
        }
      });

  agentflow::test::TestState state;  // replace with your own state proto
  state.set_user_query(question);

  agentflow::CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<agentflow::State> {
        co_return co_await node.Run(
            agentflow::State::From(std::move(state)), cancel.Token(), emit);
      },
      asio::use_future);
  io.run();

  auto out = fut.get();
  std::cout << "\n---\n"
            << out.As<agentflow::test::TestState>().assistant_reply() << "\n";
  return 0;
}
```

Use `af::test::TestState` from `//proto:agentflow_proto`, exactly as
`examples/agent-demo/main.cc:87` and `examples/streaming-demo/main.cc:90`
already do (`af` is their alias for `agentflow`). Do not define a new state
proto for this example — matching the existing examples is the convention here.

- [ ] **Step 3: Declare the example target**

Create `examples/remote-llm/BUILD.bazel`, mirroring
`examples/agent-demo/BUILD.bazel`:

```python
# examples/remote-llm/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "remote_llm",
    srcs = ["main.cc"],
    data = ["workflow.json"],
    deps = [
        "//agentflow/core",
        "//agentflow/inference/openai:openai_chat_backend",
        "//agentflow/net:https_client",
        "//agentflow/nodes",
        "//agentflow/observability",
        "//agentflow/tools",
        "//agentflow/workflow",
        "//proto:agentflow_proto",
        "@asio",
    ],
)
```

- [ ] **Step 4: Write the opt-in end-to-end test**

Create `tests/integration/remote_llm_e2e_test.cc`:

```cpp
// tests/integration/remote_llm_e2e_test.cc
//
// Opt-in end-to-end check against a real endpoint. Requires:
//   AGENTFLOW_LLM_BASE_URL, AGENTFLOW_LLM_MODEL, AGENTFLOW_LLM_API_KEY
// Skipped by default so CI stays offline and free.
#include <cstdlib>
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/inference/canonical_message.h"
#include "agentflow/inference/openai/openai_chat_backend.h"
#include "agentflow/net/https_client.h"

namespace agentflow {
namespace {

TEST(RemoteLlmE2ETest, RealEndpointStreamsAnAnswer) {
  const char* base = std::getenv("AGENTFLOW_LLM_BASE_URL");
  const char* model = std::getenv("AGENTFLOW_LLM_MODEL");
  const char* key = std::getenv("AGENTFLOW_LLM_API_KEY");
  if (!base || !model || !key) {
    GTEST_SKIP() << "AGENTFLOW_LLM_* not set";
  }

  asio::io_context io;
  net::HttpsClientOptions http_opts;
  http_opts.ca_path = "/etc/ssl/certs/ca-certificates.crt";
  net::HttpsClient http(io, http_opts);

  openai::OpenAiOptions opts;
  opts.base_url = base;
  opts.model = model;
  opts.api_key = key;
  auto backend = openai::OpenAiChatBackend::Create(opts, http);

  ChatConversationOptions conv_opts;
  conv_opts.system_message_json =
      R"([{"type":"text","text":"Answer in one short sentence."}])";
  auto conv = backend->CreateConversation(std::move(conv_opts));
  ASSERT_NE(conv, nullptr);

  std::string deltas;
  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await conv->SendAsync(
            R"({"role":"user","content":[{"type":"text","text":"Say hello."}]})",
            [&](std::string_view d) -> asio::awaitable<void> {
              deltas.append(d);
              co_return;
            },
            cancel.Token());
      },
      asio::use_future);
  io.run();

  auto resp = fut.get();
  ASSERT_TRUE(resp.ok()) << resp.status().message();
  EXPECT_FALSE(deltas.empty()) << "expected streamed deltas";
  EXPECT_EQ(deltas, ExtractAssistantText(*resp));
}

}  // namespace
}  // namespace agentflow
```

Add the corresponding `cc_test` to `tests/integration/BUILD.bazel` with
`size = "small"` and deps on `//agentflow/inference:canonical_message`,
`//agentflow/inference/openai:openai_chat_backend`,
`//agentflow/net:https_client`, `//agentflow/core`, `@asio`, and googletest.

- [ ] **Step 5: Document the host recipe**

In `README.md`, after the Architecture section, add:

````markdown
### Remote inference backends

An agent runs on whichever backend the host registers under the logical name
in its `model.backend`. On-device and cloud agents coexist in one workflow:

```json
{"agents": {
  "triage":   {"system_prompt": "...", "model": {"backend": ""}},
  "research": {"system_prompt": "...", "model": {"backend": "cloud"}}
}}
```

`triage` uses the default (on-device) backend; `research` uses the named one.
The host supplies both:

```cpp
spec.backend           = LiteRtLmChatBackend::Create(engine, io);   // default
spec.backends["cloud"] = openai::OpenAiChatBackend::Create(opts, http);
```

**Credentials never go in a workflow.** `base_url`, `api_key` and the provider
model id live only in host code — an environment variable on desktop,
`EncryptedSharedPreferences` on Android. A workflow JSON carries a logical name
and nothing else, so it stays safe to serialize, checkpoint, log and hot-push.
A name the host did not register is an error at build time, never a silent
fallback to the default backend.

Any OpenAI-compatible endpoint works: OpenAI, DeepSeek, Volcengine ARK, Kimi,
GLM, MiniMax, OpenRouter, and local Ollama / vLLM / LiteLLM gateways.

Known limitations: one connection per request (no pooling); no HTTP/2; a remote
backend cannot honour `constrained_tool_calls` and reports that through
`last_warning()`.
````

- [ ] **Step 6: Verify the example builds and the suite is green**

Run: `bazel build //examples/remote-llm:remote_llm`
Expected: builds successfully.

Run: `bazel test //tests/...`
Expected: all PASS, with the opt-in cases skipped.

- [ ] **Step 7: Run it against a real endpoint once**

```bash
export AGENTFLOW_LLM_BASE_URL=https://api.deepseek.com/v1
export AGENTFLOW_LLM_MODEL=deepseek-chat
export AGENTFLOW_LLM_API_KEY=sk-...
bazel run //examples/remote-llm:remote_llm -- "Name three primary colours."
```

Expected: the answer streams to stdout token by token, then the final reply
prints after `---`.

**This is the acceptance check for the whole plan.** If tokens appear in one
burst rather than incrementally, SSE framing or the token sink is broken —
revisit Tasks 8 and 12 before declaring done.

- [ ] **Step 8: Commit**

```bash
git add examples/remote-llm/ tests/integration/ README.md
git commit -m "docs(remote-llm): runnable example, e2e test and host recipe

Demonstrates a workflow whose agent names a logical backend the host registers,
with credentials supplied entirely by host code. Documents the credential rule
and the known limitations in README."
```

---
