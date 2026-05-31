# C++ Agent Framework — P3: MCP Tool Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the P2 tool subsystem usable with real out-of-process tools by integrating MCP (Model Context Protocol). After this plan, an `AgentNode` can invoke tools served by external MCP servers (filesystem, shell, http APIs) over stdio / HTTP-SSE / WebSocket / TCP transports, using the same `Tool` interface as `NativeFnTool`.

**Architecture:** A new `McpClient` wraps a long-lived MCP connection (one per server); `McpClientPool` keys clients by server spec so multiple `McpToolAdapter`s on the same server share one connection. Each `McpToolAdapter` implements the existing `Tool` interface and forwards `Invoke()` to the pooled client via `tools/call`. `ToolRegistry::AttachMcpServer()` discovers a server's tool list (`tools/list`), creates one adapter per remote tool, and registers them. Failure to attach a server (process spawn, handshake, schema parse) is **non-fatal**: the registry logs a warning and the missing tools are excluded from `ExportToolsJson` so the LLM never sees them.

**Tech stack:** C++20, Bazel 7.x (Bzlmod), asio 1.30 (existing), nlohmann_json (existing), [gopher-mcp](https://github.com/GopherSecurity/gopher-mcp) as the MCP client implementation (Task 1 chooses vendor-vs-minimal-client).

**Spec reference:** `docs/superpowers/specs/2026-04-27-cpp-agent-framework-design.md` §6.3, §6.4, §13 (risk R2), §15 (Demo A pure-C++ subset).

---

## File Structure

```
zen/
├── MODULE.bazel                              # MODIFY: add gopher-mcp bazel_dep / http_archive (Task 1)
├── proto/
│   ├── BUILD.bazel                           # MODIFY: add mcp_spec_proto
│   └── mcp_spec.proto                        # NEW: McpServerSpec
├── third_party/
│   └── gopher_mcp/                           # NEW (if vendored via local CMake build,
│       ├── BUILD.bazel                       #       mirrors third_party/litert_lm)
│       ├── include/...
│       └── lib/...
├── agentflow/
│   └── tools/
│       ├── BUILD.bazel                       # MODIFY: add mcp_*, link gopher-mcp
│       ├── tool.h                            # (existing)
│       ├── native_fn_tool.{h,cc}             # (existing)
│       ├── mcp_client.{h,cc}                 # NEW: wraps gopher::McpClient, lazy-start
│       ├── mcp_client_pool.{h,cc}            # NEW: keyed pool keyed by McpServerSpec
│       ├── mcp_tool_adapter.{h,cc}           # NEW: Tool impl, delegates to McpClient
│       ├── tool_registry.{h,cc}              # MODIFY: AttachMcpServer + tool exclusion
│       └── ...
└── tests/
    ├── unit/tools/
    │   ├── BUILD.bazel                       # MODIFY: add mcp_*_test
    │   ├── mcp_client_pool_test.cc           # NEW
    │   ├── mcp_tool_adapter_test.cc          # NEW (uses FakeMcpClient)
    │   ├── tool_registry_mcp_test.cc         # NEW (attach + failure semantics)
    │   └── fake_mcp_server.{h,cc}            # NEW (test helper: in-process JSON-RPC echo)
    └── integration/tools/                    # NEW (small e2e)
        ├── BUILD.bazel
        ├── stdio_mcp_smoke_test.cc           # NEW: spawn a Python echo MCP server, call a tool
        └── echo_mcp_server.py                # NEW: trivial JSON-RPC MCP server
```

## Task Dependency

```
T1 (gopher-mcp dep) ──→ T2 (proto McpServerSpec)
                  ↘
                   T3 (McpClient) ──→ T4 (McpClientPool) ──→ T5 (McpToolAdapter)
                                                                  ↓
                                                          T6 (ToolRegistry::AttachMcpServer)
                                                                  ↓
                                                          T7 (unit tests, FakeMcpClient)
                                                                  ↓
                                                          T8 (stdio integration + wrap-up)
```

T1 is the only task with significant external risk (gopher-mcp build). T7 unblocks confident iteration on T3–T6 without a real MCP server.

---

## Task 1: Vendor gopher-mcp (or commit to a minimal in-house client)

**Files:**
- Modify: `MODULE.bazel`
- (vendor path) Create: `third_party/gopher_mcp/BUILD.bazel` + lib/include after CMake prebuild
- (minimal path) move into Task 3 instead

**Context:** The spec calls for `gopher-mcp` (§6.3); §13 R2 flags it as a young project, so this task starts with a build-system survey. Two viable paths:

- **(A) Vendor gopher-mcp.** If it builds via Bazel (or CMake with reasonable deps), bring it in. Pattern mirrors `third_party/litert_lm`: prebuild out-of-tree (CMake) if Bazel support is incomplete, then `cc_import` the resulting `.a` + headers. Risk: another LiteRT-LM-style yak-shave.
- **(B) Minimal in-house MCP client.** MCP is JSON-RPC 2.0 over a transport; the surface we need is small (`initialize`, `tools/list`, `tools/call`, plus stdio line framing). ~500–800 LOC. Avoids upstream build risk.

- [ ] **Step 1.1: Probe gopher-mcp build**
  - Clone or `http_archive` gopher-mcp at the spec-pinned commit.
  - Run a Bazel `bazel build @gopher_mcp//...` smoke; if it works, vendor via Bzlmod (`bazel_dep` / `http_archive`).
  - If only CMake builds, prebuild via the LiteRT-LM pattern (`third_party/gopher_mcp/BUILD.bazel` with `cc_import`).
  - **Time-box: ≤90 min.** If neither path is clean, switch to (B).

- [ ] **Step 1.2: If choosing (B), draft the minimal client surface**

```cpp
// agentflow/tools/mcp_client.h (Task 3 will fill this in regardless of path)
namespace agentflow::mcp {
class IMcpClient {
 public:
  virtual ~IMcpClient() = default;
  virtual asio::awaitable<absl::Status> Connect() = 0;
  virtual asio::awaitable<absl::StatusOr<std::vector<ToolSchema>>> ListTools() = 0;
  virtual asio::awaitable<absl::StatusOr<std::string>> CallTool(
      std::string_view name, std::string_view args_json,
      const CancelToken& cancel) = 0;
  virtual void Shutdown() = 0;
};
}  // namespace agentflow::mcp
```

This thin interface lets Tasks 3–7 proceed identically whether the impl is `gopher::McpClient` or our own.

- [ ] **Step 1.3: Update MODULE.bazel**
  - Vendor path: `bazel_dep(name = "gopher_mcp", version = "...")` or `http_archive(name = "gopher_mcp", urls = [...], strip_prefix = "...", integrity = "...")` with a `BUILD.bazel.tpl` if upstream lacks Bazel.
  - Minimal path: no MODULE.bazel change (we use existing nlohmann_json + asio).

- [ ] **Step 1.4: Commit**

```bash
git add MODULE.bazel MODULE.bazel.lock third_party/gopher_mcp/
git commit -m "build: integrate gopher-mcp (or: scaffold minimal MCP client) for P3"
```

**Exit criterion:** `bazel build //agentflow/tools/...` still passes with the new dep declared (no consumer wired up yet).

---

## Task 2: `proto/mcp_spec.proto`

**Files:** Create `proto/mcp_spec.proto`; modify `proto/BUILD.bazel`.

- [ ] **Step 2.1: Write `proto/mcp_spec.proto`**

```protobuf
syntax = "proto3";

package agentflow.proto;

// Describes how to reach (and how to talk to) one MCP server. Used by
// ToolRegistry::AttachMcpServer and persisted in graph specs (P6 JNI).
message McpServerSpec {
  enum Transport {
    TRANSPORT_UNSPECIFIED = 0;
    STDIO = 1;       // command_or_url = executable path; args = argv[1..]
    HTTP_SSE = 2;    // command_or_url = http(s) URL
    WEBSOCKET = 3;   // command_or_url = ws(s) URL
    TCP = 4;         // command_or_url = "host:port"
  }
  Transport transport = 1;
  string command_or_url = 2;
  repeated string args = 3;           // stdio only

  // Filter: only register these tool names (empty = register all).
  repeated string include_tools = 4;
  // Filter: skip these tool names (applied after include_tools).
  repeated string exclude_tools = 5;

  // Per-call deadline (default = no deadline). 0 = use registry default.
  int32 call_timeout_ms = 6;

  // If true, defer the actual transport handshake until the first
  // ToolRegistry::Invoke() of one of this server's tools.
  bool lazy_start = 7;
}
```

- [ ] **Step 2.2: Wire into `proto/BUILD.bazel`**

```python
proto_library(
    name = "mcp_spec_proto",
    srcs = ["mcp_spec.proto"],
    strip_import_prefix = "/proto",
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "mcp_spec_cc_proto",
    deps = [":mcp_spec_proto"],
)
```

Add `:mcp_spec_cc_proto` to the existing `agentflow_proto` aggregate cc_library so downstreams see it via `//proto:agentflow_proto`.

- [ ] **Step 2.3: Build + commit**

```bash
bazel build //proto/...
git add proto/mcp_spec.proto proto/BUILD.bazel
git commit -m "proto: add McpServerSpec for MCP server attachment"
```

---

## Task 3: `agentflow/tools/mcp_client.{h,cc}` — transport wrapper + lazy stdio

**Files:** `mcp_client.{h,cc}`, modify `agentflow/tools/BUILD.bazel`.

- [ ] **Step 3.1: Write `mcp_client.h`**

```cpp
#ifndef AGENTFLOW_TOOLS_MCP_CLIENT_H_
#define AGENTFLOW_TOOLS_MCP_CLIENT_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/tool.h"  // for ToolSchema
#include "mcp_spec.pb.h"

namespace agentflow::mcp {

// One long-lived connection to one MCP server. Thread-safe enough that
// CallTool() may be invoked concurrently from different coroutines (the
// underlying transport is serialized by the io_context).
//
// The connection is brought up on the first awaited operation when
// `lazy_start` is true on the spec; otherwise Connect() is called eagerly by
// the pool.
class McpClient {
 public:
  static std::shared_ptr<McpClient> Create(
      proto::McpServerSpec spec, asio::io_context& io);
  ~McpClient();

  // Initializes the transport + performs the MCP `initialize` handshake.
  // No-op if already connected. Safe to call repeatedly.
  asio::awaitable<absl::Status> Connect();

  // `tools/list` — returns each remote tool's schema. The `name` field
  // matches the remote name (no namespacing yet).
  asio::awaitable<absl::StatusOr<std::vector<ToolSchema>>> ListTools();

  // `tools/call` — invokes a tool by name. `args_json` is forwarded as the
  // `arguments` field. Cancellation aborts the in-flight RPC; the connection
  // stays usable for subsequent calls.
  asio::awaitable<absl::StatusOr<std::string>> CallTool(
      std::string_view name, std::string_view args_json,
      const CancelToken& cancel);

  // Tears down the transport. Subsequent calls return Cancelled.
  void Shutdown();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;

  McpClient(std::unique_ptr<Impl> impl);
};

}  // namespace agentflow::mcp

#endif
```

- [ ] **Step 3.2: `mcp_client.cc` — Impl sketch**

The Impl owns one transport instance and serializes outgoing JSON-RPC requests on a per-client asio strand. Per transport:

- **STDIO**: `fork+execvp` the command (lazy or eager), connect parent's `posix::stream_descriptor` to the child's stdin/stdout, framing = newline-delimited JSON-RPC.
- **HTTP-SSE**: single POST per call (request); listen on a long-lived SSE stream for notifications/responses. (P3 may defer to a follow-up; mark in self-review.)
- **WEBSOCKET / TCP**: lower priority; implement after stdio.

Cancellation: each pending request holds an `asio::cancellation_signal`; `CallTool`'s `OnCancel` triggers the signal, which sends a JSON-RPC `notifications/cancelled` for that request id (if the server supports it) and resolves the awaitable with `kCancelled`.

Lazy start: `Connect()` is the single entry point; the impl tracks a `state_` (kIdle → kConnecting → kReady → kBroken) under the strand. All public methods `co_await` an internal `EnsureReady()` that fast-paths when `kReady`.

- [ ] **Step 3.3: BUILD.bazel — add target**

```python
cc_library(
    name = "mcp_client",
    srcs = ["mcp_client.cc"],
    hdrs = ["mcp_client.h"],
    deps = [
        "//agentflow/core",
        "//proto:agentflow_proto",
        "@asio",
        "@nlohmann_json//:json",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ] + select({
        # If Task 1 chose vendor path:
        # "//conditions:default": ["@gopher_mcp//:client"],
        "//conditions:default": [],
    }),
)
```

- [ ] **Step 3.4: Commit**

```bash
git add agentflow/tools/mcp_client.{h,cc} agentflow/tools/BUILD.bazel
git commit -m "tools: add McpClient (JSON-RPC over stdio/http-sse/ws/tcp)"
```

**Exit criterion:** `bazel build //agentflow/tools:mcp_client` passes.

---

## Task 4: `mcp_client_pool.{h,cc}`

**Files:** `mcp_client_pool.{h,cc}`, modify `agentflow/tools/BUILD.bazel`.

- [ ] **Step 4.1: Write `mcp_client_pool.h`**

```cpp
namespace agentflow::mcp {

// One McpClient per logical "server" — multiple McpToolAdapters for the same
// server share the same client. Key = (transport, command_or_url, args).
class McpClientPool {
 public:
  explicit McpClientPool(asio::io_context& io);

  // Returns an existing client for `spec` or creates one. Thread-safe.
  std::shared_ptr<McpClient> GetOrCreate(const proto::McpServerSpec& spec);

  // Closes every client (cancels in-flight calls). Safe to call on shutdown.
  void Clear();

 private:
  asio::io_context& io_;
  std::mutex mu_;
  // key = canonical serialization of (transport, command_or_url, args)
  std::unordered_map<std::string, std::shared_ptr<McpClient>> clients_;
};

}  // namespace agentflow::mcp
```

- [ ] **Step 4.2: `mcp_client_pool.cc`** — straightforward map + canonicalize the key (serialize the spec's identifying fields; `include_tools`/`exclude_tools`/`call_timeout_ms` are NOT part of the key).

- [ ] **Step 4.3: Commit**

```bash
git add agentflow/tools/mcp_client_pool.{h,cc} agentflow/tools/BUILD.bazel
git commit -m "tools: add McpClientPool (one client per logical MCP server)"
```

---

## Task 5: `mcp_tool_adapter.{h,cc}`

**Files:** `mcp_tool_adapter.{h,cc}`, modify `agentflow/tools/BUILD.bazel`.

- [ ] **Step 5.1: Write `mcp_tool_adapter.h`**

```cpp
namespace agentflow::mcp {

// Adapts one remote MCP tool into the Tool interface so it's
// indistinguishable from a NativeFnTool to the agent layer.
class McpToolAdapter : public Tool {
 public:
  McpToolAdapter(std::shared_ptr<McpClient> client,
                 std::string remote_tool_name,
                 ToolSchema cached_schema,
                 std::chrono::milliseconds call_timeout =
                     std::chrono::milliseconds{0});

  const ToolSchema& Schema() const override { return schema_; }
  asio::awaitable<std::string> Invoke(
      std::string_view args_json,
      const CancelToken& cancel) override;

 private:
  std::shared_ptr<McpClient> client_;
  std::string remote_name_;
  ToolSchema schema_;
  std::chrono::milliseconds timeout_;
};

}  // namespace agentflow::mcp
```

- [ ] **Step 5.2: `mcp_tool_adapter.cc`**

`Invoke()` delegates to `client_->CallTool(remote_name_, args_json, cancel)`, applies the per-call timeout (`asio::steady_timer` race) if set, and converts `absl::Status` failures into a JSON-encoded error result that the LLM can see, e.g.:

```json
{"isError": true, "content": [{"type": "text", "text": "tool error: <msg>"}]}
```

This matches MCP's standard error shape so the LLM-side prompt format stays uniform with successful tool results.

- [ ] **Step 5.3: Commit**

```bash
git add agentflow/tools/mcp_tool_adapter.{h,cc} agentflow/tools/BUILD.bazel
git commit -m "tools: add McpToolAdapter (Tool impl over McpClient)"
```

---

## Task 6: `ToolRegistry::AttachMcpServer`

**Files:** `tool_registry.{h,cc}`, modify `agentflow/tools/BUILD.bazel`.

- [ ] **Step 6.1: Update `tool_registry.h`**

Add to `ToolRegistry`:

```cpp
// Discovers a server's tools (via tools/list), creates one McpToolAdapter per
// remote tool, and registers them. Returns OK even if individual tools fail
// schema parsing; tools that fail are skipped with a warning. Returns a
// non-OK Status ONLY if the server itself is unreachable AND
// spec.lazy_start == false.
asio::awaitable<absl::Status> AttachMcpServer(proto::McpServerSpec spec);

// Backwards-compat: existing Register / Invoke / ExportToolsJson unchanged.
```

`ToolRegistry` gains an internal `McpClientPool pool_` and an `asio::io_context*` (passed at construction time; defaulted to nullptr to preserve the existing P2 constructor for native-only use).

- [ ] **Step 6.2: `tool_registry.cc` — AttachMcpServer flow**

```
1. client = pool_.GetOrCreate(spec)
2. if !spec.lazy_start: status = co_await client->Connect();
       if !status.ok(): log + co_return status
3. schemas = co_await client->ListTools();
       if !schemas.ok(): log + co_return InvalidArg("tools/list failed: ...")
4. for each ToolSchema s:
     if include_tools non-empty and s.name not in include_tools: skip
     if s.name in exclude_tools: skip
     Register(make_shared<McpToolAdapter>(client, s.name, s, timeout))
5. co_return OK
```

If `lazy_start` is true and `Connect()` was deferred, `ListTools()` may fail at attach time — log and **still co_return OK** so the agent graph runs without these tools. The spec's "AttachMcpServer 失败不阻塞 graph 启动" rule.

- [ ] **Step 6.3: Tool name collision policy**

If a remote tool name collides with an already-registered tool:
- Skip the MCP tool, log a warning. Native tools and earlier-attached MCP tools win. (Document in the header.)

- [ ] **Step 6.4: `ExportToolsJson` already covers MCP tools correctly** — they implement `Tool::Schema()`, so the existing export logic works without changes.

- [ ] **Step 6.5: Commit**

```bash
git add agentflow/tools/tool_registry.{h,cc}
git commit -m "tools: ToolRegistry::AttachMcpServer — discover + register MCP tools"
```

---

## Task 7: Unit tests with `FakeMcpClient`

**Files:** `tests/unit/tools/fake_mcp_server.{h,cc}`, `mcp_tool_adapter_test.cc`, `mcp_client_pool_test.cc`, `tool_registry_mcp_test.cc`, modify `tests/unit/tools/BUILD.bazel`.

- [ ] **Step 7.1: `fake_mcp_server.{h,cc}`** — an in-process `IMcpClient` implementation backed by std::functions for `ListTools` and per-tool `CallTool`. Lets us test adapter + registry behavior without subprocess plumbing.

- [ ] **Step 7.2: `mcp_tool_adapter_test.cc`**

| Test | Asserts |
|---|---|
| `InvokeReturnsResult` | `client->CallTool` returns "ok"; `Invoke` returns "ok". |
| `InvokeTimeoutTriggersError` | Slow client; per-call timeout fires → JSON `{isError:true,...}` result. |
| `InvokeCancelPropagates` | `CancelToken` flipped → adapter returns the standard "cancelled" error JSON; client sees cancel. |
| `SchemaPassThrough` | `Schema()` returns the cached schema unchanged. |

- [ ] **Step 7.3: `mcp_client_pool_test.cc`**

| Test | Asserts |
|---|---|
| `SameSpecReturnsSameClient` | Two `GetOrCreate` on identical specs return the same `shared_ptr`. |
| `DifferingSpecReturnsDifferentClient` | Different `command_or_url` ⇒ different client. |
| `FilterFieldsDoNotAffectKey` | Same transport+command, different `include_tools` ⇒ same client. |
| `ClearShutsDownAll` | After `Clear()`, the previously returned client is `Shutdown()`'d. |

- [ ] **Step 7.4: `tool_registry_mcp_test.cc`**

| Test | Asserts |
|---|---|
| `AttachRegistersRemoteTools` | Fake server with 3 tools → all 3 invocable via `Invoke()`. |
| `AttachExcludesByFilter` | `include_tools={"a"}` registers only `a`. |
| `AttachUnreachableEagerFails` | `lazy_start=false` + bad server → status != OK and no tools registered. |
| `AttachUnreachableLazySucceeds` | `lazy_start=true` + bad server → status OK, tools registered but `Invoke()` returns error JSON. |
| `LocalToolWinsOnCollision` | Pre-existing `NativeFnTool` named `t` survives an MCP-server `t`. |

- [ ] **Step 7.5: BUILD.bazel**

```python
cc_test(
    name = "mcp_tool_adapter_test",
    size = "small",
    srcs = ["mcp_tool_adapter_test.cc", "fake_mcp_server.h", "fake_mcp_server.cc"],
    deps = [
        "//agentflow/tools",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
# ...analogous targets for mcp_client_pool_test and tool_registry_mcp_test
```

- [ ] **Step 7.6: Run + commit**

```bash
bazel test //tests/unit/tools/... --test_output=errors
git add tests/unit/tools/
git commit -m "test: MCP tool adapter, client pool, and registry attach"
```

---

## Task 8: Stdio integration smoke + wrap-up

**Files:** `tests/integration/tools/echo_mcp_server.py`, `tests/integration/tools/stdio_mcp_smoke_test.cc`, `tests/integration/tools/BUILD.bazel`.

- [ ] **Step 8.1: Trivial Python MCP server** — `echo_mcp_server.py` implements JSON-RPC over stdin/stdout with one tool: `echo(text: string) → returns text`. Conforms to MCP `initialize` / `tools/list` / `tools/call`. ~80 LOC.

- [ ] **Step 8.2: `stdio_mcp_smoke_test.cc`** — Bazel `cc_test` that:
  1. Spawns the Python script via `McpServerSpec(STDIO, /usr/bin/python3, [echo_mcp_server.py])`.
  2. Calls `AttachMcpServer` (eager).
  3. Asserts `Invoke("echo", "{\"text\":\"hi\"}")` returns the expected JSON-RPC result.
  4. Cancels mid-call (slow echo variant) and asserts the standard "cancelled" error shape.

  Tagged `requires-net = "false"`, `tags = ["manual"]` initially if the host doesn't have `python3` reliably; otherwise run by default.

- [ ] **Step 8.3: Wire echo server into Bazel runfiles**

```python
py_binary(name = "echo_mcp_server", srcs = ["echo_mcp_server.py"])

cc_test(
    name = "stdio_mcp_smoke_test",
    size = "medium",
    srcs = ["stdio_mcp_smoke_test.cc"],
    data = [":echo_mcp_server"],
    deps = [
        "//agentflow/tools",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 8.4: Full verification**

```bash
bazel test //tests/unit/core/... //tests/unit/tools/... //tests/integration/tools/... \
  --test_output=errors
```

Expected: P1 core (39 cases), P2 tools (5 cases), P3 tools-unit (≈14 new cases), P3 stdio smoke (≈2 new cases) — all pass.

```bash
bazel build //examples/agent-demo:agent_demo  # still links
MODEL_PATH=models/gemma-4-E2B-it.litertlm \
  ./bazel-bin/examples/agent-demo/agent_demo
```

The agent-demo should continue to run end-to-end (untouched code path).

- [ ] **Step 8.5: Tag**

```bash
git tag -a p3-mcp -m "P3: MCP tool integration (stdio + adapter + registry attach)"
```

- [ ] **Step 8.6: Commit**

```bash
git add tests/integration/
git commit -m "test: stdio MCP smoke (spawn python echo server, call echo tool)"
```

---

## Self-Review

**Spec coverage:**
- §6.3 McpToolAdapter → Task 5
- §6.4 ToolRegistry + McpServerSpec + lazy stdio + AttachMcpServer non-fatal → Tasks 2, 4, 6
- §6.4 端侧优化 (pool, lazy-start, attach failure isolation) → Tasks 4, 6
- §13 R2 risk burn-down (probe gopher-mcp; fallback to minimal client) → Task 1

**Explicitly out of scope** (logged here for the P3 tag's release notes):
- HTTP-SSE / WebSocket / TCP transports — only stdio is exercised by Task 8. The wire format/state-machine differences live behind `McpClient::Impl` and don't perturb the public surface, so adding them later is purely additive. Track as P3.1 or fold into P4.
- MCP **resources** and **prompts** primitives (only **tools** is covered). Same isolation: future addition is a new `Tool`-shaped surface OR new registry methods.
- Server-initiated calls / sampling — not used by AgentNode's flow.
- Auth (bearer, OAuth) on HTTP transports — defer with the HTTP transport itself.

**Type consistency:**
- `Tool` / `ToolSchema` / `ToolRegistry::Register|Invoke|ExportToolsJson` are unchanged in the public P2 surface; AttachMcpServer is purely additive.
- `IMcpClient` (Task 1.2) is the only seam between the chosen client impl (gopher vs minimal) and Tasks 3–7; if Task 1 picks a different client later, only `mcp_client.cc` changes.
- All callbacks marshal onto the registry's `io_context` (same pattern as P2 fix `8bacb5f`).

**Known risks:**
1. **gopher-mcp build** (Task 1) — the headline risk. Time-boxed; fallback (minimal client) is well-scoped.
2. **Stdio process lifetime** (Task 3) — must reap the child on Shutdown / pool Clear / process exit to avoid zombies. Document in `mcp_client.cc`.
3. **JSON-RPC id collisions** under concurrent CallTool — McpClient must use a monotonic id counter under the strand.
4. **AgentNode never reads `isError`** — our LLM-side prompt currently treats tool results as plain text. The `{isError:true,...}` payload is what the LLM sees on tool failure; this is the MCP-canonical shape and the LLM can reason about it. Document; revisit in P4 when TeamNode handles tool failures programmatically.

---

## Execution Handoff

**Plan saved to `docs/superpowers/plans/2026-05-31-cpp-agent-framework-p3-mcp.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — execute tasks in this session using executing-plans, batch execution with checkpoints.

**Which approach?**
