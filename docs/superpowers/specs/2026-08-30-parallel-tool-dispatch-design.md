# Parallel Tool Dispatch — Design Spec

**Date:** 2026-08-30
**Status:** Approved design, ready for implementation planning.
**Anchors:** Builds on agentflow P1–P9. Modifies `AgentNode`'s tool-dispatch
loop so that multiple tool calls emitted by the model in a single turn run
concurrently. No new schema, no new workflow keys, no new node types.

## 1. Goal and scope

Make parallel execution of tool calls the **default behaviour** of
`AgentNode`'s dispatch loop. When the model emits N tool calls in one turn,
all N are fired concurrently and their results are collected back **in the
original order** before the single tool-role message is sent.

The motivating scenario is deep-search style workflows: a planner agent
fans out several `delegate` calls in one turn (each possibly targeting a
*different* sub-agent with a different goal), all sub-agents run
concurrently against the remote backend, and the results gather back into
the parent agent's conversation context, where the parent LLM synthesizes
the final answer.

Explicitly out of scope:

- No `parallel` flag anywhere: no proto field, no tool parameter, no
  workflow JSON key, no `AgentNodeConfig` flag. Parallelism is simply what
  the dispatch loop does.
- No turn-level scheduling or dependency analysis between tool calls. The
  model expresses ordering by placing dependent calls in different turns
  (the OpenAI `parallel_tool_calls` contract).
- No framework-side guarantees about individual tool implementations being
  concurrency-safe. Tool authors own that.
- No change to `delegate_tool.cc` or `SubAgentRuntime` — each `Invoke`
  remains an independent `RunAsync` call and is already concurrency-ready.

## 2. Core semantics

Current behaviour (`agentflow/nodes/agent_node.cc`, dispatch loop): the
tool calls of one turn are executed sequentially, one `co_await` at a time.

New behaviour: when a turn's response contains multiple `tool_calls`, each
call is spawned as its own coroutine (`asio::co_spawn`), results are
collected through one `asio::experimental::channel` per call — the same
ResultChannel pattern `TeamNode::RunParallelGather` already uses — and the
loop then awaits the channels **in tool-call order** so the tool-role
message sent back to the model preserves the original call order.

Two differences from `TeamNode::RunParallelGather`:

1. **1:1 result alignment.** `RunParallelGather` drops a member's result
   when its channel closes (member threw). Here the result array must
   correspond 1:1 to the `tool_calls` array (OpenAI semantics: each tool
   message item echoes `tool_call_id`). Any failure — tool exception,
   coroutine exception, cancellation — is therefore written as an error
   string placeholder in that call's slot, never dropped.
2. **No aggregation policy.** There is no `aggregator` function; the
   "gather" is the ordered tool-role message the model receives.

## 3. Semantic contract (load-bearing)

1. **Multiple calls in one turn ⇒ independent.** Sequential dependencies
   are expressed by the model across turns. This is the established
   OpenAI-compatible contract; remote backends already produce
   multi-call turns, and the framework currently serializes them only
   because the dispatch loop predates concurrent execution.
2. **Tool concurrency-safety is the tool author's responsibility.** The
   framework no longer guarantees sequential execution of tools within a
   turn. Stateless tools (Tavily search/extract, delegate) are naturally
   safe; a host registering a stateful tool must make it internally
   synchronized or accept interleaving.
3. **Primary target: remote OpenAI-compatible backends.** The backend is
   stateless over HTTP (one connection per request), so N concurrent tool
   calls become N concurrent HTTPS requests and the server prefills in
   parallel. This is the deep-search run mode, exercised by the cloud e2e
   test. **Secondary: the local LiteRT-LM engine** also supports
   concurrency — each session decodes on its own background thread and
   the engine is documented thread-safe — so the same workflow is run
   once against gemma-4-E2B-it on this machine as a manual wall-time
   comparison (not a CI test). Caveat: the local constrained path
   grammar-forces at most one tool call per turn, so a local deep-search
   run must be unconstrained; whether gemma-4-E2B-it reliably emits
   multi-call turns unconstrained is a validation point. If it does not,
   the workflow still runs correctly, just sequentially.
4. **1:1 ordering invariant.** The tool-role message always contains
   exactly one result per tool call, in the original order, regardless of
   completion order or individual failures.

## 4. Changes

### 4.1 `agentflow/nodes/agent_node.cc` — dispatch loop (the only code change)

Replace the sequential `for` loop over `resp["tool_calls"]` with:

- Parse and normalize all tool calls first (name, call id, args) exactly
  as today — the untrusted-input hardening stays. Malformed calls (not an
  object, unreadable name/args) are skipped during normalization, exactly
  as the current loop does, and never enter the spawn list; the 1:1
  alignment invariant applies only to normalized calls.
- One `ResultChannel` per normalized call; `co_spawn` each dispatch; a
  try/catch wrapper around each coroutine closes the channel on exception
  (mirrors `RunParallelGather`).
- Await channels in order; a closed channel (or error status) yields the
  placeholder string `{"error":"tool_execution_failed"}` so the result
  array keeps its 1:1 alignment.
- The existing per-call `DispatchTool` wrapper (which already catches tool
  exceptions into `"Tool error: ..."` strings and emits
  `EmitToolCall`/`EmitToolReturn`) is reused unchanged for each spawned
  coroutine.
- `CancelToken` is passed into every spawned coroutine; the existing
  conversation-level cancel hook is unchanged.

Note on token streaming: when several `delegate` calls run concurrently,
each already creates its own sub-channel and drains to the top channel
(`delegate_tool.cc`), so concurrent token streams interleave safely — no
change needed.

### 4.2 `proto/workflow_spec.proto` — comment only

Rewrite the burned-field-3 comment on `DelegateSpec`: parallel execution
is the dispatch loop's default behaviour and needs no declaration; the
`parallel` key therefore remains reserved/rejected because it has no
meaning (there is nothing to enable, and no switch to disable).

### 4.3 `agentflow/workflow/workflow_loader.cc` — message only

Keep rejecting a static `"parallel"` key in `delegates`, but reword the
error: parallelism is the default and needs no such key.

### 4.4 Untouched

`delegate_tool.cc`, `sub_agent_runtime.{h,cc}`, `workflow_runner.cc`,
`team_node.*`, all protos beyond the comment above, schema_version stays
at 1 (behaviour change for multi-call turns is intentional; single-call
turns are byte-for-byte unchanged).

## 5. Concurrency safety argument

The Runner is single-threaded (one `io_context`); "parallel" means
interleaved coroutines, not multiple threads, so there is no data race:

- `JsonlEventEmitter` already holds an internal mutex and is safe for
  interleaved `Emit` calls.
- `CallbackEventEmitter` is documented to rely on the single-threaded
  Runner — unchanged and not violated.
- `HttpsClient` opens one `Connection` per request (no shared mutable
  per-request state) — concurrent POSTs are safe.
- `SubAgentRuntime::RunAsync` creates a fresh conversation per call; the
  OpenAI-compatible backend is stateless over HTTP (history resent per
  turn), so concurrent conversations are safe.
- Cancellation propagates through the shared `CancelToken` to every
  spawned coroutine and to the in-flight HTTP requests via the existing
  `Connection` close hooks.

## 6. Testing plan

1. **AgentNode dispatch tests** (fake backend returning a multi-call
   turn):
   - All tool calls execute concurrently (verified via an event-order /
     interleaving assertion, e.g. a tool that yields and records order).
   - Results appear in the tool-role message in the original call order,
     one per call, with `tool_call_id` echoed correctly.
   - A throwing tool yields an error placeholder in its slot; sibling
     results are unaffected and not shifted.
   - Cancellation mid-flight propagates to all spawned coroutines.
   - A single-call turn behaves exactly as today (no behavioural diff).
2. **`workflow_loader_test.cc`**: keep the static-`parallel` rejection
   test; update the expected error substring to the reworded message.
3. **Cloud e2e test** (`tests/integration/deep_search_e2e_test.cc`,
   following the `remote_llm_e2e_test.cc` pattern): gated on
   `AGENTFLOW_LLM_BASE_URL` / `AGENTFLOW_LLM_MODEL` (plus optional
   `AGENTFLOW_LLM_API_KEY` / `AGENTFLOW_LLM_CA_PATH`) and
   `TAVILY_API_KEY`; `GTEST_SKIP()` when unset so CI stays offline.
   Runs the real deep-search workflow end to end against a real cloud
   endpoint with real Tavily traffic. Asserts **structure, not model
   prose**: the run completes, every delegate call returned a non-error
   result (no `{"error":...}` placeholders), the final answer field is
   non-empty, and — the parallel-specific assertion — the observed
   sub-agent wall times overlap (spawned concurrently) rather than
   summing to the total, so a regression to sequential dispatch is
   detectable.
4. **Local wall-time comparison** (manual, not CI): run the same
   workflow against `models/gemma-4-E2B-it.litertlm` (engine default,
   20-core CPU host) and record wall time vs. the cloud run. Documents
   the local/remote trade-off and empirically answers whether
   gemma-4-E2B-it emits multi-call turns unconstrained.

## 7. Deep-search deliverables

A runnable example under `examples/deep-search/`:

- `workflow.json` — two agents:
  - `deep_search` (main): system prompt instructs it to split the query
    into sub-questions, emit one `delegate` call per sub-question **in a
    single turn** (this is what makes them run concurrently), then
    synthesize a cited answer from the gathered results.
  - `searcher`: one sub-question at a time; uses `tavily_search` +
    `tavily_extract`; returns a concise result with source URLs.
  - `delegates` roster lists the sub-agents available (a roster of
    *different* sub-agents is supported — each task may differ in agent,
    tools, and goal).
- `tavily_tools.{h,cc}` — two `NativeFnTool`s over the existing
  `HttpsClient` (POST only; both Tavily endpoints are POST):
  `tavily_search` (POST `/search`) and `tavily_extract` (POST `/extract`,
  accepts multiple URLs in one call so page reading fans out server-side).
- `main.cc` — host wiring with **dual backends selected by environment**:
  - Cloud (default): reads `AGENTFLOW_LLM_BASE_URL` / `AGENTFLOW_LLM_MODEL`
    / `AGENTFLOW_LLM_API_KEY` / `AGENTFLOW_LLM_CA_PATH`, builds
    `OpenAiChatBackend`, registers it under the logical name
    `workflow.json` references.
  - Local (when `MODEL_PATH` is set): builds `LiteRtLmEngine` +
    `LiteRtLmChatBackend` from `models/gemma-4-E2B-it.litertlm` and
    registers it under the same logical name, so the identical
    `workflow.json` runs unchanged — one binary, two run modes for the
    wall-time comparison.
  - Registers `tavily_search` / `tavily_extract` (`TAVILY_API_KEY` from
    the environment; credentials never in the workflow JSON), loads the
    workflow, runs the agent, prints timing per phase.
- `BUILD.bazel`.

## 8. Explicit non-goals (YAGNI)

- No dynamic fan-out primitives in `Runner`/`Graph` (no per-item subgraph
  expansion, no new node types).
- No `TeamNode` policy changes or dynamic member factories.
- No concurrency limit knobs, no per-turn parallel-batch grouping
  semantics (all calls in a turn are one implicit batch).
- No `schema_version` bump.
