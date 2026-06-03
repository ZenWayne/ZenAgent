# P7 — Checkpoint hook + manual resume

**Branch:** `feat/p7-checkpoint`
**Spec anchor:** §1.4 feature #8 (P1), §4.5 `CheckpointPolicy`, §15 acceptance #5 ("Demo B: researcher 完成后 `kill -9`，重启用 checkpoint manual resume 能从 reviewer 节点继续").
**Goal:** Make a Runner that can write its state to disk after each node completes, and a Runner.Resume API that lets a fresh process pick up from a saved checkpoint without re-running completed nodes.

Spec calls for sqlite-backed `CheckpointWriter`. For v1 we ship a binary proto file writer — same interface, sqlite can land as a swap-in implementation later. Spec is explicit: **no auto-resume.** The caller must explicitly load a checkpoint and call `Runner::Resume`. This avoids the failure mode where stale state silently restarts after code changes.

## What we have today

- `State::SerializeAsString()` / `ParseFromString()` already round-trips the protobuf state.
- `Runner::Options` only has `trace`. No checkpoint policy yet.
- Runner's `activations_` map tracks pending inputs and completed nodes internally, but exposes none of it. Each node coroutine emits `NODE_END` and fan-out edges; we'll piggyback the checkpoint write right after `NODE_END`.

## Design

### Checkpoint shape

```proto
// agentflow/proto/checkpoint.proto
syntax = "proto3";
package agentflow.proto;

message Checkpoint {
  // Serialized user State protobuf (same bytes State::SerializeAsString gives).
  bytes state_bytes = 1;
  // Fully-qualified protobuf message type of the state (e.g.
  // "agentflow.test.TestState"). Used at resume time to construct the right
  // empty State target before ParseFromString.
  string state_type = 2;
  // Node IDs that have already completed. Resume skips these.
  repeated string completed_nodes = 3;
  // Wall-clock when this snapshot was written, for debugging.
  int64 unix_micros = 4;
  // Optional schema version — bumped if checkpoint format ever changes
  // incompatibly. Resume refuses checkpoints with newer versions.
  uint32 schema_version = 5;
}
```

### CheckpointWriter interface

```cpp
// agentflow/persist/checkpoint_writer.h
class CheckpointWriter {
 public:
  virtual ~CheckpointWriter() = default;

  // Persist a checkpoint synchronously. Returning a Status (not throwing) so
  // a write failure during Run doesn't take the graph down — the Runner
  // logs and continues. The next checkpoint write replaces this one.
  virtual absl::Status Write(const proto::Checkpoint& cp) = 0;
};

// agentflow/persist/file_checkpoint_writer.h
class FileCheckpointWriter : public CheckpointWriter {
 public:
  explicit FileCheckpointWriter(std::string path);
  absl::Status Write(const proto::Checkpoint& cp) override;
 private:
  std::string path_;
  std::mutex mu_;  // serialize writes; Runner is single-threaded but safer.
};

// agentflow/persist/checkpoint_reader.h
absl::StatusOr<proto::Checkpoint> ReadCheckpointFromFile(
    const std::string& path);
```

`FileCheckpointWriter` writes to a `.tmp` sibling then atomically renames over the target. That way a `kill -9` mid-write doesn't leave a half-written checkpoint to be loaded later.

### Runner.Options additions

```cpp
struct Options {
  EventEmitter* trace = nullptr;

  enum class CheckpointPolicy {
    Off,
    AfterEachNode,        // write after every NODE_END
    AfterTerminalNode,    // write only on graph completion (cheaper, less fidelity)
  };
  CheckpointPolicy checkpoint_policy = CheckpointPolicy::Off;
  CheckpointWriter* checkpoint_writer = nullptr;  // non-owning; required if policy != Off
};
```

### Resume API

```cpp
// On the Runner:
asio::awaitable<State> Resume(
    const proto::Checkpoint& cp,
    State target,           // empty State of the correct user type
    CancelToken cancel = {});
```

Why caller-provided `target` and not deserialize inline? `State` is constructed via `State::From<T>(T{})`. The Runner doesn't know what `T` is. The caller does. Pattern:

```cpp
auto cp = *ReadCheckpointFromFile("graph.ckpt");
State target = State::From(test::TestState{});
auto out = co_await runner.Resume(cp, std::move(target), cancel);
```

Inside Resume:
1. `target.ParseFromString(cp.state_bytes())` — fill state from bytes.
2. Mark every `cp.completed_nodes()` as in_flight=false + completed in activations_.
3. For each completed node, walk its outgoing edges and seed successor `pending_inputs` with `target.Clone()` (treating the loaded state as that node's output). Decrement `remaining_all` accordingly.
4. Dispatch any nodes that are now satisfied.
5. Continue normal `Run` loop.

This is the simplest resume that works for the linear acceptance demo (researcher done → reviewer next). For fan-in (multiple predecessors), a single checkpoint can't perfectly reconstruct per-edge pending inputs — we document this as a v1 limitation and recommend `AfterTerminalNode` for fan-in-heavy graphs.

## File Structure

```
agentflow/
  proto/
    checkpoint.proto                       # NEW
    BUILD.bazel                            # modify — register new proto
  persist/                                 # NEW directory
    checkpoint_writer.h
    file_checkpoint_writer.{h,cc}
    checkpoint_reader.{h,cc}
    BUILD.bazel
  core/
    runner.{h,cc}                          # modify — add Options.checkpoint_*, write hook, Resume
tests/unit/
  persist/                                 # NEW
    file_checkpoint_writer_test.cc
    checkpoint_reader_test.cc
    BUILD.bazel
  core/
    runner_test.cc                         # modify — add resume tests
examples/
  checkpoint-demo/                         # NEW
    main.cc                                # 3-node linear graph; --resume <path> flag
    BUILD.bazel
docs/superpowers/plans/
  2026-06-04-cpp-agent-framework-p7-checkpoint.md   # this file
```

## Task Dependency

```
T1 (proto) ─┐
            ├─→ T2 (writer/reader libs) ─→ T3 (Runner write hook) ─→ T4 (Runner.Resume) ─→ T5 (tests) ─→ T6 (demo) ─→ T7 (tag + PR)
```

## Task 1: `Checkpoint` proto

**Files:** `agentflow/proto/checkpoint.proto`, `agentflow/proto/BUILD.bazel`

- Write the proto exactly as designed above.
- Register in the existing `agentflow_proto` proto_library + cc_proto_library targets (or a sibling target if cleaner).
- Build target. No code under it yet.

Commit: `feat(p7): add Checkpoint proto`

## Task 2: `CheckpointWriter` + `FileCheckpointWriter` + `ReadCheckpointFromFile`

**Files:** `agentflow/persist/`

### Step 2.1: Headers + impl

- `checkpoint_writer.h` — pure-virtual interface.
- `file_checkpoint_writer.{h,cc}` — writes to `path + ".tmp"` then `std::rename`. Locks `mu_` for the write+rename critical section.
- `checkpoint_reader.{h,cc}` — `absl::StatusOr<proto::Checkpoint> ReadCheckpointFromFile(path)`. Returns `NotFoundError` if file doesn't exist; `DataLossError` if parse fails. Validates `schema_version <= kCurrentVersion`.

### Step 2.2: BUILD

`cc_library` with deps on `//proto:agentflow_proto`, `@abseil-cpp//absl/status:statusor`.

### Step 2.3: Commit

`feat(p7): persist/ — CheckpointWriter interface + FileCheckpointWriter + reader`

## Task 3: Runner write hook

**Files:** `agentflow/core/runner.{h,cc}`

### Step 3.1: Options

Add `CheckpointPolicy` enum + `checkpoint_policy` + `checkpoint_writer` fields to `Runner::Options`. Validate in Runner ctor: if `policy != Off`, `writer` must be non-null (throw `AgentflowError` otherwise).

### Step 3.2: Internal completed-set tracking

In `Runner::Impl`, add `std::vector<std::string> completed_node_ids_` (under `mu_`). Push on every `NODE_END` (not on failure — a failed node isn't "done" for checkpoint purposes; resume would skip it forever).

### Step 3.3: Write hook

After `emit_.EmitNodeEnd(...)` in the coro lambda, if policy is `AfterEachNode`:
- Build a `proto::Checkpoint`: serialize the most recent terminal-state-equivalent into `state_bytes`. **Wrinkle:** there is no single "current state" in a DAG until the terminal node. Solution: snapshot the last successfully-produced `out` State from the just-completed node (this is the state that flowed through that node's coroutine, available as `out` right before fan-out).
- Fill `state_type` from `out.UnsafeMessage()->GetTypeName()`.
- Fill `completed_nodes` from `completed_node_ids_`.
- Call `writer_->Write(...)`. On error, log + continue (do not throw).

`AfterTerminalNode` policy: same but only when `terminal_state_` is set (last node finished).

### Step 3.4: Commit

`feat(p7): Runner writes checkpoint per CheckpointPolicy`

## Task 4: `Runner::Resume`

**Files:** `agentflow/core/runner.{h,cc}`

### Step 4.1: Public signature

```cpp
asio::awaitable<State> Resume(
    const proto::Checkpoint& cp,
    State target,
    CancelToken cancel = CancelToken());
```

### Step 4.2: Implementation

1. `target.ParseFromString(cp.state_bytes())` — fail with `AgentflowError` if false.
2. For each `node_id` in `cp.completed_nodes()`:
   - Verify the node exists in the graph (else `AgentflowError`).
   - Mark `activations_[node_id].in_flight = false`, push it onto `completed_node_ids_`.
   - Walk outgoing edges; for each successor, push `target.Clone()` into `pending_inputs` and decrement the appropriate ALL/ANY counter — same code path as the post-Run fan-out, but with the loaded `target` as the "result."
3. Set `pending_initial_inputs_` to empty (we don't need to re-seed entry nodes).
4. Dispatch any nodes whose dependencies are now satisfied. The normal `Run` loop continues.
5. Return the terminal state.

### Step 4.3: Edge cases doc'd in the header

- Resume from a checkpoint with `completed_nodes` empty == cold start with the loaded state as initial.
- Resume from a checkpoint where every node is completed == returns `target` immediately (graph already done).
- Resume after a graph topology change (e.g. new node added between two existing ones) is undefined behavior — caller must use the same Graph at resume as at checkpoint time.

### Step 4.4: Commit

`feat(p7): Runner::Resume — manual checkpoint resume`

## Task 5: Tests

### Step 5.1: persist unit tests

- `FileCheckpointWriter` round-trip: write, read back via `ReadCheckpointFromFile`, assert all fields match.
- Atomic rename: simulate "kill mid-write" by injecting a slow serializer — assert the file is either fully written or absent, never partial.
- `ReadCheckpointFromFile` returns `NotFoundError` for missing path.
- `ReadCheckpointFromFile` returns `DataLossError` for corrupt file.

### Step 5.2: Runner write-hook tests

In `runner_test.cc`, add:
- `WritesCheckpointAfterEachNode` — 3-node linear graph, AfterEachNode policy, `RecordingCheckpointWriter` (test double) captures every `Write` call. Assert 3 writes, each carries the right `completed_nodes`.
- `WritesNoCheckpointWhenPolicyIsOff` — same graph, Off policy, writer never called.

### Step 5.3: Runner resume tests

- `ResumeContinuesFromMidGraph` — run a 3-node linear `(a→b→c)` graph; intercept checkpoint after `b` completes; build a fresh Runner with the same graph; call `Resume(cp, target)`; assert only `c` runs (not `a` or `b` — verified via a shared counter).
- `ResumeWithAllCompletedReturnsImmediately` — checkpoint says all 3 done; Resume returns `target` with no node execution.
- `ResumeRejectsUnknownNodeInCheckpoint` — corrupt checkpoint with bogus node id → `AgentflowError`.

### Step 5.4: Commit

`test(p7): persist + Runner checkpoint/resume coverage`

## Task 6: Demo

**Files:** `examples/checkpoint-demo/`

Linear `start → planner → researcher → writer` graph with `StubNode` members. Each node sleeps briefly and bumps state.counter.

CLI:
- No args: run from scratch, write checkpoint to `/tmp/p7_demo.ckpt` after each node, exit after writer completes. Print final state.
- `--resume /tmp/p7_demo.ckpt`: read checkpoint, build same graph, call `Resume`, print final state.
- `--kill-after <node_id>`: after the named node completes its NODE_END, `std::abort()` so the checkpoint is the last thing on disk. This simulates the "kill -9 after researcher" path.

Verification script:
```bash
./checkpoint_demo --kill-after researcher   # crashes after researcher
./checkpoint_demo --resume /tmp/p7_demo.ckpt
# expected: writer is the only node that runs; final counter == 4 (the same as a clean run)
```

Commit: `examples(p7): checkpoint-demo with --kill-after + --resume flags`

## Task 7: Tag + PR

- `git tag p7-checkpoint`
- Push branch, open PR against master.
- PR title: `feat(p7): checkpoint hook + manual resume`
- PR body: link to plan, list scope vs spec, call out limitations (no sqlite, no fan-in mid-graph resume guarantees).

Commit: `chore: tag p7-checkpoint`

## Self-Review

**Why proto over JSON or sqlite?**
Proto serialization is already in the codebase; round-trips via `Message::SerializeToString` / `ParseFromString`. JSON adds a parser dep and is fragile for binary state bytes. Sqlite is overkill for a single-row write-on-NODE_END use case. If a future user needs multi-graph durability or queryability, swap in a `SqliteCheckpointWriter` behind the same interface.

**Why `target` parameter for Resume?**
`State::ParseFromString` needs a target message of the right concrete protobuf type already constructed (it calls `msg_->ParseFromArray`). We could route through proto descriptor lookup + dynamic message creation, but that pulls in `DescriptorPool` machinery and is fragile when test_messages.proto isn't in the default pool. Pushing the empty target to the caller keeps the API straightforward — they already know `T` because they built `State::From<T>` at start time.

**Risks:**
- DAG fan-in resume isn't perfectly defined. If two predecessors fan into one successor and only one is checkpointed as completed, resume's seed of `target.Clone()` into successor's `pending_inputs` is correct for the completed branch but doesn't reflect the other branch's potentially-different state. Document as a known limitation; AfterTerminalNode policy sidesteps it.
- State type drift: if the user changes their state proto between checkpoint and resume, parse may succeed but with stale/missing fields. We can't detect this without a hash; document as caller's responsibility (the explicit "no auto-resume" decision in the spec is partly about this).
- `std::rename` atomicity on the same filesystem is POSIX-guaranteed; cross-filesystem we'd fall through to copy+unlink which isn't atomic. Document: checkpoint_path must be on the same FS as its `.tmp` sibling.

**Out of scope (YAGNI):**
- Automatic resume on Runner construction.
- Sqlite backend.
- Compression / encryption.
- Per-node checkpoint hooks (Node.WriteCheckpoint).
- Mid-coroutine resume (we only resume at node boundaries; in-flight LLM decode is restarted from scratch on the next node, not from token offset).

## Execution Handoff

Resume by reading this file and Task 1. Each task ends in a commit. After T5, `bazel test //...` must be green before T6. T6's visual verification (the `--kill-after`/`--resume` round trip producing the same final counter as a clean run) is the v1 acceptance proof.
