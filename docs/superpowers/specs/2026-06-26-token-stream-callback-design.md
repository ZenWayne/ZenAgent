# Token streaming: collapse channels into a callback chain

**Date:** 2026-06-26
**Branch:** `token-stream-callback` (merges into `android-arm64-inference`)
**Status:** approved, implementing

## Problem

The token-output chain from C++ generation to the Android UI carries **three**
separate "callback ↔ buffer" adapters, two of which exist for no strong reason:

1. `LiteRtLmConversation::channel_` (ASIO channel) — bridges the engine's
   synchronous C streaming callback into the coroutine world. **Load-bearing.**
2. `agentflow::TokenChannel` (ASIO `concurrent_channel`, capacity 4096) —
   carries text deltas from `AgentNode` (and each delegated sub-agent) up to the
   JNI bridge. Sub-agents each get a *per-call* channel plus a drain coroutine
   and an end-of-stream sentinel that forwards up to the run-wide channel.
3. Kotlin `LinkedBlockingQueue<Item>` + daemon worker thread + `Item` sealed
   ADT + DONE sentinel + hand-rolled emit loop in `NativeTokenStream.kt` —
   turns the blocking, callback-based native call into a cold `Flow<String>`.

Adapters #2 and #3 are the maintenance burden the user flagged ("底层又 channel，
kotlin 又是 queue"). Both can collapse:

- **#2** only adds fan-in and back-pressure on top of what is already a callback
  at the JNI→JVM boundary (`env->CallVoidMethod(onToken, …)`). Fan-in is inherent
  to a callback (many-to-one), and back-pressure is unused (the downstream Kotlin
  queue is unbounded, so the 4096-slot channel never fills). So the channel buys
  nothing here that a plain callback doesn't.
- **#3** is a hand-rolled `callbackFlow`. The standard-library `callbackFlow{}`
  provides the internal buffer and `awaitClose` lifecycle directly.

`#1` is left untouched: it bridges the engine's *synchronous* callback into the
graph's coroutine executor; removing it would mean rewriting the executor.

## Design

### C++ side: `TokenChannel*` → `TokenSink` callback

Introduce a single canonical sink type (rename `agentflow/core/token_channel.h`
→ `token_sink.h`):

```cpp
namespace agentflow {
// Called once per generated text delta, in order, on the io thread that runs
// the agent graph. Many-to-one: the main agent and every delegated sub-agent
// share one sink — fan-in is inherent to a callback. Empty = don't stream.
using TokenSink = std::function<void(std::string_view delta)>;
}
```

Thread it through, replacing every `TokenChannel*`:

| File | Before | After |
|------|--------|-------|
| `agent_node.h` | `TokenChannel* token_channel` | `TokenSink on_delta` |
| `agent_node.cc` | `co_await token_channel->async_send(delta)` | `if (cfg_.on_delta) cfg_.on_delta(delta);` (synchronous, no suspension) |
| `workflow_runner.{h,cc}` | `spec.token_channel` | `spec.token_sink`; `cfg.stream_tokens = (bool)spec.token_sink` |
| `sub_agent_context.h` | `TokenChannel* token_channel` | `TokenSink token_sink` |
| `sub_agent_runtime.cc` | build a `try_send` adapter onto `ctx.token_channel` | pass `ctx.token_sink` straight to `send()` (already the right type) |
| `delegate_tool.{h,cc}` | per-call `sub_ch` + drain coroutine + sentinel; `io` + `top_channel` params | `sub_ctx.token_sink = top_sink;` — drain/sentinel/`io` param all deleted |
| `jni/agentflow_jni.cc` | `TokenChannel channel`; producer + consumer `co_spawn`; `channel.close()` | one `co_spawn` for the run; `on_delta` lambda calls `CallVoidMethod` inline |

Net deletions: the ASIO channel type, the delegate drain coroutine + sentinel,
and the JNI dual-coroutine choreography.

`SubAgentRuntime::TokenSink` is already `std::function<void(std::string_view)>`;
the new top-level `TokenSink` is the same type, so the sub-agent leaf needs no
adapter — `ctx.token_sink` is passed directly into `send()`.

### Kotlin side: hand-rolled queue → `callbackFlow`

`NativeTokenStream.kt` keeps a daemon worker thread (the native call is blocking
and must not run on the collector's dispatcher), but the `LinkedBlockingQueue`,
the `Item` ADT, the DONE sentinel, and the manual emit loop are replaced by
`callbackFlow`'s internal channel + `awaitClose`. The worker delivers deltas via
`trySendBlocking` and signals completion with `close()` / `close(cause)`.

## Invariants to preserve (regression guards)

1. **No cancel-on-completion deadlock (ZenAgent#24).** `nativeCancel` must fire
   **only** when the collector cancels mid-stream — never on normal/error
   completion, where re-entering the cancel path on a torn-down run/io_context
   deadlocks. `callbackFlow`'s `awaitClose` runs on *every* termination, so guard
   with an `AtomicBoolean done` the worker sets before it closes; `awaitClose`
   sends `nativeCancel` only when `!done`. `nativeFreeCancel` stays an idempotent
   release in the worker's `finally`.
2. **Kotlin callback throwing stops forwarding.** After `CallVoidMethod`, the
   JNI `on_delta` checks `env->ExceptionCheck()`; on a pending exception it sets
   a `cb_failed` flag (leaving the exception pending) and becomes a no-op for the
   rest of the run. After `io.run()` returns, `ExceptionCheck()` → return null so
   the exception surfaces to the caller. (Strictly better than today: `on_delta`
   is a cheap no-op once failed, so there is no >4096-token producer-stall risk
   that the old full-channel path had.)

## Back-pressure (preserved, and actually improved)

The C++ side no longer has its own bounded channel, but back-pressure is not
lost — it moves to the Kotlin edge and becomes end-to-end. The worker delivers
deltas with `trySendBlocking` into `callbackFlow`'s bounded buffer; when the
collector falls behind, `trySendBlocking` blocks the worker thread, which blocks
the JNI `CallVoidMethod`, which blocks `on_delta`, which blocks the io thread —
so native generation naturally pauses until the UI drains. The collector runs on
a separate thread, so it keeps draining and there is no deadlock; on collector
cancellation the channel closes and `trySendBlocking` returns immediately.

This replaces the old design's *unbounded* `LinkedBlockingQueue` (which applied
no back-pressure at all), so memory under a slow collector is now bounded rather
than growing without limit.

## Threading note

Everything — main agent, every sub-agent (`RunAsync` co_awaits under the
caller's io_context, no nested `io.run()`), and all `on_delta` callbacks — runs
on the single io thread that drives the run. Execution is cooperative (one
coroutine between suspension points), so the synchronous `CallVoidMethod` inside
`on_delta` is race-free and order-preserving.

There are two JNI entry points, with different threads behind `on_delta`:

- **`runJsonWorkflowStreaming`** (fresh per-call engine): the JNI calling thread
  itself drives `io.run()`, so `on_delta` runs on that JVM thread and uses `env`
  directly (no `AttachCurrentThread`).
- **`nativeSessionSendMessage`** (persistent multi-turn session): the run is
  driven by the session's own worker thread, so `on_delta` runs there. That
  thread is attached to the JVM for the turn (a global ref to the callback plus
  a `tenv` set at the start of the run coroutine and cleared at the end), and the
  JNI calling thread blocks on a `done` latch until the turn finishes. Collapsing
  the old producer/consumer pair into one coroutine preserves this exactly —
  only the intervening `TokenChannel` is gone.

## Testing

- `SubAgentRuntimeTest.StreamsDeltasToChannel` → rewritten to collect deltas into
  a `std::vector` via a `TokenSink` (drain coroutine + sentinel deleted).
- New/extended coverage: normal streaming completion; mid-stream cancel (asserts
  no deadlock / Flow completes); callback-throws path; multi-agent delegate
  fan-in ordering.
- Build both Bazel (`//agentflow/...`, `//jni:...`) and the CMake unit-test
  target; run the workflow/nodes unit suites.
