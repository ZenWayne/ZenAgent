# P5 — Trace Channel Completeness + Reusable Event Sinks

**Branch:** `feat/p5-trace-sink`
**Spec anchor:** §4.7 Event/Trace, §1.4 feature #6 (streaming) and #9 (trace API)
**Goal:** Make the trace event channel actually usable end-to-end. Today
producers emit *some* events (`TOKEN`, `EDGE_FIRE`, `NODE_FAILED`, `GRAPH_DONE`)
but **never** emit `NODE_START`, `NODE_END`, `TOOL_CALL`, `TOOL_RETURN`, and
the only available sink is `NullEventEmitter`. Every test and demo ships its
own ad-hoc emitter. After P5 the trace channel is complete on both ends:
producers emit every documented event, and consumers can pick from three
production-grade sinks plus compose them.

P5 does **not** add new node types, JNI, Kotlin DSL, checkpoints, or constrained
decoding — those are P6+. P5 is the "make what we have actually observable"
phase that unblocks (a) honest demos, (b) future JNI hooks, and (c) debugging
the LiteRT-LM inference blocker tracked in `memory/project_litert_lm_build.md`.

---

## File Structure

```
agentflow/
  core/
    runner.cc                       # MODIFY — emit NODE_START/NODE_END around node->Run
  nodes/
    agent_node.cc                   # MODIFY — emit TOOL_CALL/TOOL_RETURN in HandleToolCall
  observability/                    # NEW directory
    jsonl_event_emitter.{h,cc}      # NEW — one JSON line per event to std::ostream
    callback_event_emitter.{h,cc}   # NEW — wraps std::function<void(const TraceEvent&)>
    multi_event_emitter.{h,cc}      # NEW — fan-out to N child emitters
    BUILD.bazel                     # NEW
tests/unit/
  core/
    runner_test.cc                  # MODIFY — replace ad-hoc CapturingEmitter, add NODE_START/END assertions
    event_test.cc                   # MODIFY — replace ad-hoc CapturingEmitter
  nodes/
    agent_node_test.cc              # MODIFY — add tool emission test
  observability/                    # NEW
    jsonl_event_emitter_test.cc
    callback_event_emitter_test.cc
    multi_event_emitter_test.cc
    BUILD.bazel
examples/
  core-stub-graph/main.cc           # MODIFY — drop StdoutEmitter, use JsonlEventEmitter
  team-demo/main.cc                 # MODIFY — drop StdoutEmitter, use JsonlEventEmitter
  trace-cat/                        # NEW (optional) — small CLI that reads JSONL trace + pretty-prints
    main.cc
    BUILD.bazel
docs/superpowers/plans/
  2026-06-02-cpp-agent-framework-p5-trace-sink.md   # this file
```

---

## Task Dependency

```
T1 (Runner NODE_START/END) ─┐
T2 (AgentNode TOOL events)  ─┤
T3 (JsonlEventEmitter)      ─┤
T4 (CallbackEventEmitter)   ─┼─→ T6 (test sweep + reuse) ──→ T7 (examples sweep) ──→ T8 (tag + PR)
T5 (MultiEventEmitter)      ─┘
```

T1–T5 are independent and could be parallelised. T6 depends on all of them
because the test sweep wants to use the library sinks. T7 follows T6. T8 is
the wrap-up.

---

## Task 1: Runner emits `NODE_START` / `NODE_END`

**File:** `agentflow/core/runner.cc`

### Step 1.1: Add `EmitNodeStart` immediately before `node->Run`

In the `asio::co_spawn` body around line ~155 (right where `State out;
bool failed = false;` is declared), call `emit_.EmitNodeStart(node_id_view)`
**before** the `try`. The node id view is already in scope.

### Step 1.2: Add `EmitNodeEnd` in both branches

After the `try`/`catch` block, before the fan-out logic, emit:

```cpp
emit_.EmitNodeEnd(node_id_view, /*cancelled=*/cancel.IsCancelled(),
                  /*failed=*/failed);
```

Cancellation flag must be checked *after* `node->Run` returns, because the node
itself may have observed cancel mid-execution and returned partial state.

### Step 1.3: Verify ordering

The expected event sequence for a successful linear `a → b` graph becomes:

```
NODE_START a → NODE_END a (cancelled=0,failed=0) → EDGE_FIRE a→b
NODE_START b → NODE_END b → GRAPH_DONE(failed=0)
```

For a failure on `a`: `NODE_START a → NODE_FAILED a → NODE_END a (failed=1) → GRAPH_DONE(failed=1)`.
NODE_FAILED is emitted **inside** the `catch` (already there); NODE_END follows.

### Step 1.4: Commit

`feat: runner emits NODE_START/NODE_END around node->Run`

---

## Task 2: AgentNode emits `TOOL_CALL` / `TOOL_RETURN`

**File:** `agentflow/nodes/agent_node.cc`

### Step 2.1: Thread `EventEmitter&` into `HandleToolCall`

`HandleToolCall` is called from `Run`, which already has `EventEmitter& emit`
in scope. Extend the helper's signature:

```cpp
asio::awaitable<void> HandleToolCall(
    State& state, const std::string& call_id, const std::string& name,
    const std::string& args, const CancelToken& cancel, EventEmitter& emit);
```

Update the declaration in `agent_node.h` (line 54 area) and the call site at
line ~196 to pass `emit`.

### Step 2.2: Emit around the dispatch

In `HandleToolCall` (around line 224 in the current file):

```cpp
emit.EmitToolCall(Id(), name, args);
std::string result;
try {
  result = co_await cfg_.tool_registry->Invoke(name, args, cancel);
} catch (const std::exception& e) {
  result = std::string("Tool error: ") + e.what();
}
emit.EmitToolReturn(Id(), name, result);
```

Even error paths emit a `TOOL_RETURN` — the result string carries the error
text. Consumers diffing call/return pairs always see a closing event.

### Step 2.3: Commit

`feat: AgentNode emits TOOL_CALL/TOOL_RETURN around dispatch`

---

## Task 3: `JsonlEventEmitter`

**Files:** `agentflow/observability/jsonl_event_emitter.{h,cc}`

### Step 3.1: Header

```cpp
// agentflow/observability/jsonl_event_emitter.h
#ifndef AGENTFLOW_OBSERVABILITY_JSONL_EVENT_EMITTER_H_
#define AGENTFLOW_OBSERVABILITY_JSONL_EVENT_EMITTER_H_

#include <iosfwd>
#include <mutex>

#include "agentflow/core/event.h"

namespace agentflow {

// Writes each TraceEvent as a single JSON line to `out`. Thread-safe via an
// internal mutex; the caller may share one emitter across threads (e.g. when
// the same trace sink is consumed by Runner + a sibling coroutine).
//
// Format: protobuf JSON serialization with `add_whitespace=false` and
// `always_print_primitive_fields=true` for stable diffing. One '\n' per event.
class JsonlEventEmitter : public EventEmitter {
 public:
  explicit JsonlEventEmitter(std::ostream& out);
  void Emit(proto::TraceEvent ev) override;

 private:
  std::ostream& out_;
  std::mutex mu_;
};

}  // namespace agentflow

#endif
```

### Step 3.2: Implementation

```cpp
// agentflow/observability/jsonl_event_emitter.cc
#include "agentflow/observability/jsonl_event_emitter.h"

#include <ostream>
#include <string>

#include <google/protobuf/util/json_util.h>

namespace agentflow {

JsonlEventEmitter::JsonlEventEmitter(std::ostream& out) : out_(out) {}

void JsonlEventEmitter::Emit(proto::TraceEvent ev) {
  std::string line;
  google::protobuf::util::JsonPrintOptions opts;
  opts.add_whitespace = false;
  opts.always_print_primitive_fields = true;
  (void)google::protobuf::util::MessageToJsonString(ev, &line, opts);
  std::lock_guard<std::mutex> lk(mu_);
  out_ << line << '\n';
}

}  // namespace agentflow
```

`MessageToJsonString` returns a status — we ignore it because TraceEvent has
no unknown fields and conversion can't realistically fail. If it does, the
event is silently dropped (acceptable for telemetry).

---

## Task 4: `CallbackEventEmitter`

**Files:** `agentflow/observability/callback_event_emitter.{h,cc}`

```cpp
// callback_event_emitter.h
class CallbackEventEmitter : public EventEmitter {
 public:
  using Callback = std::function<void(const proto::TraceEvent&)>;
  explicit CallbackEventEmitter(Callback cb);
  void Emit(proto::TraceEvent ev) override;
 private:
  Callback cb_;
};
```

Implementation: stores `cb_`, `Emit` calls `cb_(ev)` if non-null. **No
internal locking** — the callback contract documents that callers must be
thread-safe if they hand the emitter to a multi-threaded driver. (Our Runner
is single-threaded today; this leaves the door open for the JNI bridge that
wants to marshal events onto a Kotlin Flow's executor.)

---

## Task 5: `MultiEventEmitter`

**Files:** `agentflow/observability/multi_event_emitter.{h,cc}`

```cpp
class MultiEventEmitter : public EventEmitter {
 public:
  // Children are non-owning. Must outlive this emitter. Order = emit order.
  explicit MultiEventEmitter(std::vector<EventEmitter*> children);
  void Emit(proto::TraceEvent ev) override;
 private:
  std::vector<EventEmitter*> children_;
};
```

`Emit` copies `ev` for the first N-1 children and moves into the last (the
prior calls each `Emit(ev)` by value to preserve the proto, then for the last
call `Emit(std::move(ev))` to avoid one copy). If `children_` is empty, no-op.

---

## Task 6: Unit tests + sweep ad-hoc emitters

### Step 6.1: New tests

- `tests/unit/observability/jsonl_event_emitter_test.cc`:
  - Emit one TOKEN event; parse the line via nlohmann::json; assert
    `kind == 3` and `token.token == "hi"`.
  - Emit two events; assert the output has exactly two `\n`-separated lines.
  - Thread-safety: two threads emit 1000 events each into a `std::stringstream`;
    assert 2000 lines, no interleaving (each line parses as valid JSON).
- `tests/unit/observability/callback_event_emitter_test.cc`:
  - Construct with a lambda capturing `std::vector<TraceEvent>`. Emit 3
    events. Assert vector size == 3 and order preserved.
  - Default-constructed (null callback) — Emit is a silent no-op (no crash).
    *Decision:* require non-null at construction; document that nullptr is UB.
    Then drop the no-op assertion.
- `tests/unit/observability/multi_event_emitter_test.cc`:
  - Two child `CallbackEventEmitter`s capturing into separate vectors. Emit 5
    events. Assert both vectors have 5 events in matching order.
  - Empty children → no-op (no crash).

### Step 6.2: Runner test extension

In `tests/unit/core/runner_test.cc`:
- Remove the local `CapturingEmitter` class. Replace with a vector + a
  `CallbackEventEmitter` that pushes into it.
- Add `RunnerTest, EmitsNodeStartEndForLinearGraph` — runs a 2-node graph,
  asserts the event sequence includes NODE_START(a), NODE_END(a,
  cancelled=0, failed=0), EDGE_FIRE(a,b), NODE_START(b), NODE_END(b),
  GRAPH_DONE(failed=0) in that order.
- Add `RunnerTest, EmitsNodeEndWithCancelledFlag` — node that observes
  cancel and returns partial state; assert NODE_END has cancelled=1.
- Add `RunnerTest, EmitsNodeEndWithFailedFlag` — node that throws; assert
  NODE_FAILED precedes NODE_END(failed=1).

### Step 6.3: event_test sweep

Remove the duplicate `CapturingEmitter` in `tests/unit/core/event_test.cc`.
Replace with the same `CallbackEventEmitter` pattern. Tests should still pass
unchanged in intent.

### Step 6.4: AgentNode test extension

In `tests/unit/nodes/agent_node_test.cc`:
- `AgentNodeTest, EmitsToolCallAndToolReturn` — register a stub tool, drive
  one ReAct cycle, assert TOOL_CALL(name, args) precedes TOOL_RETURN(name,
  result) with matching node_id == agent's Id().

### Step 6.5: BUILD + run all tests + commit

Add `observability/BUILD.bazel` and `tests/unit/observability/BUILD.bazel`.
Run `bazel test //...` with the proxy args and assert all pass.

Commit: `test: trace-event sinks + Runner/AgentNode emission coverage`

---

## Task 7: Examples sweep

### Step 7.1: `examples/core-stub-graph/main.cc`

Replace the local `StdoutEmitter` class with:

```cpp
af::JsonlEventEmitter trace(std::cout);
Runner::Options opts; opts.trace = &trace;
```

### Step 7.2: `examples/team-demo/main.cc`

Same swap.

### Step 7.3: (Optional) `examples/trace-cat/`

Tiny CLI: reads JSONL from stdin or argv[1], parses each line as a
TraceEvent via `JsonStringToMessage`, prints a human-readable line. Useful
docs/demo for the trace format. Skip if Bazel build feels heavyweight.

### Step 7.4: Build + verify both demos still produce expected output

```
bazel run //examples/core-stub-graph:main 2>&1 | head -5
bazel run //examples/team-demo:main 2>&1 | head -5
```

Visual check that JSON lines are well-formed and the counter/last_node final
state still matches what P4 produced.

Commit: `examples: drop ad-hoc emitters, use JsonlEventEmitter library`

---

## Task 8: Tag + PR

- `git tag p5-trace-sink`
- Push branch + open PR against master.
- PR title: `feat(p5): trace event completeness + reusable sinks`
- PR body: list the 4 emission gaps closed and 3 sink classes added; link to
  this plan.

Commit: `chore: tag p5-trace-sink`

---

## Self-Review

**Why this scope, not bigger?**
The user's P5 pick was "Streaming + trace sink." Streaming-the-bytes was
already wired (LlmNode + AgentNode both call `EmitToken`). The gap is the
*event channel as a whole*. JNI/Kotlin (option B) would dwarf this and
shouldn't be conflated. Checkpoint (option C) is orthogonal. We stay scoped
to "make the event channel honest and composable."

**Why no `ChannelEventEmitter` (asio channel)?**
Discussed and deferred. Real consumers today are synchronous (file, stderr,
callback). The asio channel variant only earns its keep when there's a
coroutine downstream that wants to `co_await` on events — and that consumer
doesn't exist yet. Add it in P6 if JNI needs it.

**Why `MultiEventEmitter` takes raw pointers, not shared_ptr?**
Existing Runner takes `EventEmitter* trace` (non-owning). Matching that
convention keeps ownership explicit at the call site. Lifetime is the
caller's job, just like with `Runner::Options::trace`.

**Why protobuf JSON, not nlohmann?**
The TraceEvent oneof has rich nested types (FinalPayload carries bytes,
ToolCall has nested fields). Protobuf JSON handles oneofs, bytes (base64),
and enums correctly with zero hand-rolled code. We already depend on
protobuf; nlohmann would need a manual switch on `kind`.

**Risk: `always_print_primitive_fields = true` makes events verbose.**
Acceptable. The output is a debug/observability channel, not a wire format.
Stable field presence helps diffing and downstream parsers.

**Risk: `MessageToJsonString` ignores errors.**
TraceEvent has no map<>, no unknown fields, no Any. The conversion is
infallible in practice. If it ever fails, dropping one event is preferable
to crashing the agent.

**Out of scope (explicit YAGNI):**
- Perfetto / OpenTelemetry sinks (spec calls them out but they're consumer
  glue, not framework).
- Trace event filtering / sampling.
- Async/batched flushing in JsonlEventEmitter.
- A separate `EventListener` interface — we already have `EventEmitter`.

---

## Execution Handoff

Resume by reading this file and Task 1. Each task ends in a commit; do not
batch. After T6, `bazel test //...` must be all green before T7. After T7,
visually verify both demos. T8 tags, pushes, opens PR — wait for user to
merge.
