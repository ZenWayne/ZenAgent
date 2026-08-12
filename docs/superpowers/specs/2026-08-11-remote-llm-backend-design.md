# Remote LLM Backend — Design Spec

**Date:** 2026-08-11
**Status:** Approved design, ready for implementation planning.
**Anchors:** Builds on agentflow P1–P9 and the dynamic orchestration layer
(2026-06-06). Introduces an inference-engine seam so a cloud LLM and the
on-device LiteRT-LM engine are interchangeable within one workflow.

## 1. Goal and scope

Make a remote, OpenAI-compatible LLM endpoint a **first-class inference
backend**, usable side by side with on-device LiteRT-LM in the same workflow:
simple nodes run the local small model, complex nodes run a cloud large model.

This is not a development-time stand-in and not a fallback path. A remote
backend must get the full agent feature set — ReAct loop, tool dispatch,
sub-agent delegation, mid-decode cancellation, token streaming — with no
feature reimplemented for it.

### In scope

- An abstract `IChatBackend` / `IConversation` seam in `agentflow/inference/`.
- `LiteRtLmChatBackend` — the existing engine behind the new seam.
- `OpenAiChatBackend` — OpenAI-compatible `/v1/chat/completions`, streaming.
- A minimal HTTPS client (`agentflow/net/`) built on asio + BoringSSL.
- Host-injected backend instances, selected per agent by logical name.
- CI-runnable unit tests for the mapping layer, and — as a deliberate
  side benefit — for `AgentNode` itself.

### Out of scope

- Anthropic Messages and Gemini `generateContent` protocols. The seam admits
  them later; the first version implements only the OpenAI shape, which
  already covers OpenAI, DeepSeek, Volcengine ARK, Kimi, GLM, MiniMax,
  OpenRouter, Ollama, vLLM and LiteLLM gateways.
- Connection pooling, HTTP/2, redirect following.
- Automatic local↔cloud failover or routing policy. Backend choice is
  declared per agent, not decided at runtime.
- Credentials in the workflow spec (see §5).

## 2. Why this shape

`SubAgentRuntime` already contains the right abstraction, one layer too high.
`agentflow/workflow/sub_agent_runtime.h:43` defines:

```cpp
using SendFn = std::function<asio::awaitable<absl::StatusOr<std::string>>(
    const std::string& message_json, const TokenSink& on_token,
    const ::agentflow::CancelToken& cancel)>;
```

with the comment *"This is the only LLM operation RunAsync performs — the
entire injected surface"*, and a `ConversationFactory` that production fills
with a real engine and tests fill with a fake. The repository has already
proved that **one conversation turn** is the correct granularity for the seam.

This design promotes that contract from the workflow layer to the inference
layer and writes a second implementation behind it. It does not invent a new
interface.

Two alternatives were considered and rejected:

- **A parallel `RemoteLlmNode`.** Zero risk to existing code, but it cannot
  deliver the goal: a cloud model could then only run a feature-poor node,
  never `AgentNode`. It also duplicates ~300 lines of ReAct/tool/cancellation
  logic and violates README boundary rule 1.
- **A stateless `Complete(system, tools, messages[])` seam.** Smaller and a
  natural fit for HTTP, but it forces the local path to resend full history
  each turn, discarding LiteRT-LM's cross-turn KV-cache reuse. An 8-iteration
  ReAct loop would become 8 full prefills on device.

## 3. The seam

New file `agentflow/inference/chat_backend.h`. Depends on neither LiteRT-LM
nor HTTP.

```cpp
namespace agentflow {

// One text delta. Returns an awaitable and is always co_awaited by its
// caller, so a consumer that is not keeping up back-pressures the decode loop
// instead of having its tokens dropped. (This settles a pre-existing
// inconsistency: AgentNode awaited its channel send while SubAgentRuntime
// used try_send and dropped on a full channel.)
using TokenSink =
    std::function<asio::awaitable<void>(std::string_view delta)>;

struct ChatConversationOptions {
  // A bare content array, NOT a {role,content} object:
  //   [{"type":"text","text":"You are ..."}]
  // LiteRT-LM wraps it into {role:system, content:<this>} itself
  // (see AgentNode::BuildSystemMessageJson).
  std::string system_message_json;
  std::string tools_json = "[]";
  std::string messages_json = "[]";
  int max_output_tokens = 1024;

  // LiteRT-only: LLGuidance grammar constraint. A remote backend cannot
  // honour it; it emits a trace warning and proceeds unconstrained rather
  // than degrading silently (see §6).
  bool constrained_tool_calls = false;
};

// One multi-turn conversation. The implementation owns history: locally the
// engine does (KV-cache reuse), remotely an internal messages array does.
class IConversation {
 public:
  virtual ~IConversation() = default;

  // Send one message JSON, await the full canonical assistant JSON.
  // When on_token is set, each text delta is delivered as it arrives.
  // The return value is always the canonical shape, backend-independent.
  virtual asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) = 0;

  // Break the in-flight request. Safe from any thread.
  virtual void Cancel() = 0;
};

class IChatBackend {
 public:
  virtual ~IChatBackend() = default;
  virtual std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) = 0;
  // For traces and error messages, e.g. "litert-lm" or
  // "openai:deepseek-chat". Never contains credentials.
  virtual std::string_view Describe() const = 0;
};

}  // namespace agentflow
```

### 3.1 The canonical message shape

The canonical shape is what LiteRT-LM already returns. Nothing new is
invented; the remote implementation adapts to it.

```json
{"role":"assistant",
 "content":[{"type":"text","text":"..."}],
 "tool_calls":[{"id":"call_abc",
                "function":{"name":"search","arguments":"{...}"}}]}
```

`AgentNode` already parses both `tc["name"]` and `tc["function"]["name"]`
(`agent_node.cc:218`), and OpenAI uses the latter, so **tool dispatch needs no
change**.

### 3.2 Tool-call id passthrough

The current tool-result message is Gemma-template specific and carries only a
name:

```json
{"role":"tool","content":[{"name":"search","response":{"value":"..."}}]}
```

OpenAI requires every tool result to carry the `tool_call_id` of the call it
answers. The canonical shape therefore gains an optional `id` on each content
entry, copied by `AgentNode` from the tool call it is answering:

```json
{"role":"tool","content":[{"id":"call_abc","name":"search",
                           "response":{"value":"..."}}]}
```

`OpenAiConversation` reads `id` to restore `tool_call_id`. On the LiteRT path
the Gemma jinja template reads only `name` and `response`, so the extra field
is ignored — **zero behavioural change on device**. The change to `AgentNode`
is roughly three lines in the dispatch loop (`agent_node.cc:215-241`).

Rejected alternatives: matching ids back by tool name is ambiguous when the
same tool is called twice in one turn; positional pairing is fragile and
implicit.

### 3.3 Implementations

| | `LiteRtLmChatBackend` | `OpenAiChatBackend` |
|---|---|---|
| Construction | wraps existing `LiteRtLmEngine` + `io_context&` | `{base_url, api_key, model, io_context&, IHttpClient&}` |
| History | owned by the engine (KV-cache reuse) | internal `messages[]` array |
| `SendAsync` | contains the ~90 lines of stream-envelope parsing currently inside `AgentNode` | HTTPS POST `/v1/chat/completions` + SSE parse |
| `Cancel` | existing `conv->Cancel()` | closes the socket |

### 3.4 A net simplification of `AgentNode`

`SendAsync` unifies streaming and non-streaming into one method, so the two
large branches in `AgentNode::Run` (`agent_node.cc:117-197`) collapse into one
call. About 90 lines of LiteRT-specific stream-envelope parsing move out of
the generic node and into `LiteRtLmChatBackend`, where they belong.
**`AgentNode` gets shorter, not longer.**

## 4. The HTTPS client

New directory `agentflow/net/`, depending only on asio and BoringSSL — nothing
from agentflow. The MCP client's HTTP-SSE transport (currently
`Unimplemented`) can reuse it later.

```cpp
namespace agentflow::net {

struct HttpRequest {
  std::string url;   // https://host[:port]/path
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

// One SSE frame's data payload, with "data: " stripped. [DONE] is not
// delivered; it terminates the stream.
using SseHandler = std::function<void(std::string_view data)>;

class IHttpClient {
 public:
  virtual ~IHttpClient() = default;
  virtual asio::awaitable<absl::Status> PostSse(
      HttpRequest, const SseHandler&, const CancelToken&) = 0;
  virtual asio::awaitable<absl::StatusOr<std::string>> Post(
      HttpRequest, const CancelToken&) = 0;
};

// Production implementation.
class HttpsClient : public IHttpClient {
 public:
  HttpsClient(asio::io_context&, HttpsClientOptions);  // ca_path, timeouts
  ...
};

}  // namespace agentflow::net
```

`OpenAiChatBackend` takes an `IHttpClient&`, not a concrete `HttpsClient`, so
the mapping layer is testable without a network. This matches the two
injection seams already in the repository (`McpClientPool::ClientFactory`,
`SubAgentRuntime::ConversationFactory`).

**Scope, deliberately narrow:** POST only; HTTP/1.1 only; chunked and SSE
response bodies. No redirect following (cloud APIs do not need it), no
connection pooling (a fresh connection per request — a documented limitation
of this version), no HTTP/2.

### 4.1 TLS integration — resolved by a spike, not by assumption

`asio::ssl` is written against the OpenSSL API, and BoringSSL is only
partially compatible with it. BoringSSL reports `OPENSSL_VERSION_NUMBER` as
1.1.1, so asio takes its 1.1.1 code path and will most likely compile, but
neither project promises compatibility.

**The first implementation step is a ~30-line compile probe.** If
`asio::ssl` + BoringSSL compiles and completes a handshake, use it. If not,
fall back to driving BoringSSL manually: an asio raw TCP socket plus
`SSL_set_bio` with memory BIOs, using only `SSL_read`/`SSL_write`/`BIO_*`,
which are stably OpenSSL-compatible in BoringSSL. That is roughly 150 extra
lines.

The `IHttpClient` interface is identical either way, so this risk is confined
to one file and blocks nothing else in the design.

Verified available: `@boringssl//:ssl` exists with `ssl.h`, `bio.h` and
`x509.h`; the repository already links `@boringssl//:crypto` for workflow
signing (`agentflow/workflow/BUILD.bazel:126`).

### 4.2 Certificate verification

A `ca_path` option accepting either a bundle file (desktop:
`/etc/ssl/certs/ca-certificates.crt`) or a hashed directory (Android:
`/system/etc/security/cacerts/`). There is no option to skip verification.

### 4.3 Message mapping

| canonical (LiteRT shape) | OpenAI |
|---|---|
| `system_message_json` = `[{"type":"text","text":S}]` (bare array) | `{"role":"system","content":S}`, concatenating all text items |
| `{"role":"user","content":[{"type":"text","text":U}]}` | `{"role":"user","content":U}` |
| `tools_json` | passed through unchanged — already the OpenAI shape |
| `{"role":"tool","content":[{id,name,response:{value:R}},…]}` | **expanded one-to-many** into N × `{"role":"tool","tool_call_id":id,"content":R}` |
| ← `choices[0].message.content` | `{"role":"assistant","content":[{"type":"text","text":C}]}` |
| ← `choices[0].message.tool_calls` | copied verbatim into canonical `tool_calls` — shapes already agree and ids are present |

The one-to-many expansion is the sole reason §3.2 exists: a single canonical
tool message may carry several results, and OpenAI requires each to be its own
message with its own `tool_call_id`.

`BuildToolsJson` (`agent_node.cc:49`) already emits
`{"type":"function","function":{name,description,parameters}}`, which is
exactly the OpenAI tools shape — no conversion needed.

### 4.4 SSE stream parsing

Frames split on `\n\n`; `data:` lines are read; `[DONE]` ends the stream.
Per frame:

- `choices[0].delta.content` → pushed to `TokenSink` immediately (which is
  what feeds the existing `emit.EmitToken` and `token_channel` paths) and
  accumulated.
- `choices[0].delta.tool_calls[i]` → **arguments arrive fragmented across
  frames** and are joined by `index`. The `id` and `function.name` appear only
  in that index's first frame; later frames carry only `function.arguments`
  fragments requiring string concatenation. This is the most error-prone part
  of OpenAI streaming and is covered by dedicated tests (§7).

When the stream ends, a canonical assistant JSON is assembled. What
`AgentNode` receives is byte-for-byte the same shape as the local path
produces; it cannot tell which backend ran.

## 5. Configuration and credentials

Credentials, `base_url` and provider model names **never enter the workflow
spec**. They would otherwise flow into serialization, checkpoints and logs —
a real leak surface on Android.

- `proto ModelSpec` gains `string backend = 3;`, a **logical name only**
  (e.g. `"cloud-big"`). Empty means the default backend.
- `WorkflowSpec.engine` becomes `backend` (the default
  `shared_ptr<IChatBackend>`) plus
  `map<string, shared_ptr<IChatBackend>> backends` — logical name to
  instance, populated by the host at startup.
- The host constructs `OpenAiChatBackend` directly, reading its key from
  wherever it belongs on that platform (an environment variable on desktop,
  `EncryptedSharedPreferences` on Android).
- A `ModelSpec.backend` naming a logical backend absent from the map is a
  **load-time error**, reported by `workflow_loader` alongside its other
  validation failures. It is not silently resolved to the default backend:
  falling back from an intended cloud model to a local one would change
  answer quality invisibly.

Accepted trade-off: a workflow JSON alone does not reveal which provider an
agent runs against — only the logical name. Resolution lives in host code.
This was chosen deliberately over putting provider details in the spec.

### 5.1 Call-site changes

- `AgentNodeConfig.engine` / `LlmNodeConfig.engine` →
  `std::shared_ptr<IChatBackend> backend`.
- `SubAgentRuntime::DefaultConversationFactory(engine, io)` →
  `ConversationFactory(std::shared_ptr<IChatBackend>)`, reduced to a
  three-line wrapper around `backend->CreateConversation`. **The `SendFn`
  contract and `RunAsync` do not change at all**, so every existing fake-based
  test stays valid.
- `SubAgentRuntime::TokenSink` becomes a type alias of the new
  `agentflow::TokenSink` rather than a second identical declaration. The
  signature is unchanged, so this is source-compatible for all callers.

## 6. Error handling

`SendAsync` returns `absl::StatusOr<std::string>`, following the existing
convention.

| Condition | Status | Retried |
|---|---|---|
| Connection failure, TLS handshake failure, timeout | `Unavailable` | yes |
| HTTP 429 | `ResourceExhausted` | yes |
| HTTP 5xx | `Unavailable` | yes |
| HTTP 4xx (401 bad key, 400 bad request) | `PermissionDenied` / `InvalidArgument` | no |
| Response JSON unparseable or wrong shape | `Internal` | no |
| Cancelled | `Cancelled` | no |

**Retry has one hard constraint.** Exponential backoff (3 attempts, 100 ms
base) is permitted **only while no token has been emitted**. Once `TokenSink`
has fired even once, a mid-stream failure is reported immediately rather than
retried — otherwise the user sees a duplicated partial answer in the UI. This
constraint gets its own test.

**Credentials never escape.** `Describe()` returns a credential-free string;
error messages scrub the `Authorization` header before concatenation; trace
events record the backend description, never the request body.

**Cancellation** reuses the existing chain unchanged: `agent_node.cc:108`
registers `cancel.OnCancel([conv]{ conv->Cancel(); })`; the remote
implementation closes its socket and the in-flight `co_await` resolves as
`Cancelled`.

**`constrained_tool_calls` against a remote backend** does not degrade
silently. The backend emits a trace warning stating that grammar-constrained
decoding is unsupported there, then proceeds with ordinary function calling.
Silently dropping a correctness guarantee is worse than reporting it.

## 7. Testing

**1. Regression protection.** Every fake-based case in
`sub_agent_runtime_test.cc` must keep passing with **no assertion changed**.
An unchanged `SendFn` contract is this design's central premise, and those
tests are its verification.

The one permitted edit is mechanical: because `TokenSink` now returns
`asio::awaitable<void>` (§3), the streaming fake's `on_token(...)` call becomes
`co_await on_token(...)` and its enclosing lambda becomes a coroutine. Any
change beyond that signals the contract really did drift.

**2. Mapping and stream parsing (new, CI-runnable, no network).** With
`FakeHttpClient` injected, covering the error-prone cases:

- tool-call `arguments` split across frames are rejoined correctly;
- several tool calls in one frame are merged by `index`;
- one canonical tool message expands into multiple OpenAI messages each
  carrying the right `tool_call_id`;
- text deltas and tool-call deltas interleaved in one stream;
- retry is suppressed once a token has been emitted (§6).

**3. HTTP/SSE pure parsing (new, CI-runnable, no socket).** Response-header
parsing, chunked decoding and SSE frame splitting are extracted as pure
functions and tested directly. Socket and TLS behaviour becomes an **opt-in
integration test** (endpoint from an environment variable, `GTEST_SKIP` by
default), since it cannot be asserted in an offline CI environment.

**4. `AgentNode` gains CI unit tests.** All three cases in
`agent_node_test.cc` currently `GTEST_SKIP` unless `MODEL_PATH` is set
(`:47`, `:85`, `:166`), so `AgentNode`'s 291 lines of ReAct loop, tool
dispatch and iteration limiting have no CI protection today. Once `AgentNode`
accepts an `IChatBackend`, a fake backend makes those cases runnable without a
model file. This is not the feature's goal but it is nearly free, and it is
included in scope.

**5. End-to-end.** One opt-in manual test against a real endpoint with a key
from the environment, running a full ReAct turn with a tool call. Skipped by
default.

## 8. Implementation order

1. TLS spike (§4.1) — decides `HttpsClient`'s internals, blocks nothing else.
2. `chat_backend.h` seam; `LiteRtLmChatBackend` wrapping today's behaviour;
   migrate `AgentNode`, `LlmNode`, `SubAgentRuntime`, `WorkflowSpec` onto it.
   Existing tests must stay green.
3. Fake backend for `AgentNode`; un-skip the three existing cases (§7.4).
4. `agentflow/net/` — pure parsers first, then `HttpsClient`.
5. `OpenAiChatBackend` — mapping, SSE assembly, retry policy, with the §7.2
   tests.
6. `ModelSpec.backend` plumbing, host-side backend registry, docs.

## 9. Known limitations

- One TCP connection per request; no pooling. Acceptable for turn-based agent
  traffic, revisit if latency proves to matter.
- Only the OpenAI-compatible protocol. Anthropic and Gemini need a second
  adapter behind the same seam.
- A remote backend cannot honour `constrained_tool_calls`.
- A workflow spec alone does not identify the concrete provider behind a
  logical backend name (§5, accepted deliberately).
