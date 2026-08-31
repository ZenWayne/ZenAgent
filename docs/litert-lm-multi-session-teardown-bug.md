# LiteRT-LM Multi-Session Teardown Bug — Investigation Report

**Date:** 2026-08-31
**Status:** Open — vendored engine defect, framework-side mitigations shipped.
**Scope:** Local (on-device LiteRT-LM) runs only. Cloud backends are unaffected.

## 1. Summary

Running a workflow that creates and destroys several LiteRT-LM sessions in
one process (e.g. deep-search: one planner conversation + N searcher
conversations, each its own session) can abort at process exit with
`free(): invalid next size`. The crash is inside the vendored LiteRT-LM
engine's teardown path — `EngineImpl::~EngineImpl` destroying a
`TensorBuffer` hash map — and is reachable from framework code only as a
trigger; the defect itself is in the pinned engine submodule.

Two distinct engine-layer failures were observed. One is fully mitigated in
framework code; the other is not.

| # | Symptom | Trigger | Status |
|---|---------|---------|--------|
| 1 | `Cannot auto-resize tensor embeddings: no dims_signature exists` → backend send fails → uncaught `AgentflowError` → abort | Multiple live sessions (overlapping prefill/decode) | **Mitigated** — engine-wide serialization slot (`67605d4`) |
| 2 | `free(): invalid next size` at engine teardown, after `SessionBasic::CancelProcess` | ≥4 session create/destroy lifecycles in one process | **Open** — engine-side defect |

## 2. Symptoms

### 2.1 Multi-session prefill failure (mitigated)

```
ERROR: [litert_compiled_model.cc:205] Failed to register input tensor buffer:
ERROR: [...litert/runtime/compiled_model.cc:1214]
└ Cannot auto-resize tensor embeddings: no dims_signature exists
terminate called after throwing an instance of 'agentflow::AgentflowError'
  what():  AgentNode: backend send failed: litert-lm: streaming send failed mid-decode
```

Observed when a second session began prefill while the first session's
lifecycle was still in flight on the engine.

### 2.2 Teardown double-free (open)

```
ThisI0000 ... session_basic.h:131] SessionBasic::CancelProcess
I0000 ... threadpool.cc:46] ThreadPool 'engine': Shutting down...
free(): invalid next size (normal)
Aborted (core dumped)   # exit 134 / SIGABRT
```

`CancelProcess` at the very end is the main thread tearing down the last
conversation; the abort happens while the engine destructor walks its
`TensorBuffer` map.

## 3. Core-dump evidence (coredumpctl, PID 384167)

Frames (abridged):

```
#0-6  libc free/abort path (heap check failure)
#7   LiteRtTensorBuffer::~LiteRtTensorBuffer()
#8   LiteRtDestroyTensorBuffer
#9-13 absl raw_hash_set<FlatHashMapPolicy<string_view, litert::TensorBuffer>>
        ::destructor_impl → destroy_slots (per-slot destructor loop)
#14  litert::lm::LlmLiteRtCompiledModelExecutorBase::~...()
#15  LlmLiteRtCompiledModelExecutorStatic::~...
#16  litert::lm::EngineImpl::~EngineImpl()
#17  litert_lm_engine_delete
#18  agentflow::LiteRtLmEngine::~LiteRtLmEngine()
#19-23 shared_ptr release chain
#24  agentflow::LiteRtLmChatBackend::~LiteRtLmChatBackend()
#31  main
```

Reading: at process exit, `main` releases the backend, which releases the
engine; `EngineImpl`'s destructor destroys the compiled-model executor,
which iterates a `string_view → TensorBuffer` map and frees each buffer.
One of those buffers was already freed (or its header corrupted), so
`free()` aborts. The corruption is produced earlier, during session
lifecycles — a session destroy path leaves the engine's TensorBuffer
bookkeeping inconsistent.

## 4. Trigger matrix (empirical, gemma-4-E2B-it, 20-core x86_64)

| Scenario | Sessions created | Result |
|---|---|---|
| `agent-demo` (single tool call) | 1 | Clean exit |
| deep-search, simple query (1 delegation) | 2 | Clean exit — full correct answer, 23.1 s |
| deep-search, 3-sub-question query (3 searchers) | 4 | Abort at exit with `free(): invalid next size` |
| Same workflow on cloud backend | n/a | Clean |

So the teardown defect scales with session lifecycle count: two sessions
survive, four do not.

## 5. Framework-layer mitigations already shipped

1. **Quote-token decoding** (`34fba14`): gemma-4-E2B leaks its
   `open_quote`/`close_quote` special token into unconstrained tool-call
   arguments (`<|"|>`), which made every `delegate` call fail with
   `unknown_agent` and hid the engine issues entirely. `DecodeGemmaQuoteTokens`
   strips the token in `AgentNode` and `SubAgentRuntime`. Without this fix
   the engine bugs were unreachable.

2. **Engine-wide serialization slot** (`67605d4`): a capacity-1 channel
   shared by all conversations a `LiteRtLmChatBackend` creates; each
   `SendAsync` acquires it before prefill/decode and releases it on every
   exit path (RAII). At most one conversation is mid-prefill/decode at any
   moment, so engine failure #1 (overlapping sessions) cannot occur. Tool
   calls (HTTP) still run concurrently outside the lock, so cloud-side
   fan-out behaviour is unchanged.

What serialization deliberately does NOT do: it cannot make the engine's
session *destroy* path correct. Sessions are still created and destroyed
sequentially, and that sequence alone corrupts the engine's TensorBuffer
bookkeeping.

## 6. Root-cause hypotheses (framework-observable, engine-side)

- The engine keeps a global `TensorBuffer` registry (per compiled model).
  Session teardown frees buffers; the registry apparently retains stale
  entries, so `EngineImpl::~EngineImpl` frees the same buffer twice.
- Alternatively a session-destroy path frees a buffer but the map's
  destructor expects a different owning convention, corrupting the free-list
  header (`invalid next size`).
- Confirming either requires instrumenting the vendored engine (allowed to
  READ, not to modify — submodule is pinned per repo boundary rules).

## 7. Repro

```bash
cd /home/wayne/tools/zen
cp examples/deep-search/env.example env.local   # fill real TAVILY_API_KEY
set -a; source env.local; set +a
export ANDROID_NDK_HOME=/opt/android-sdk/ndk/28.2.13676358
export MODEL_PATH=models/gemma-4-E2B-it.litertlm

# Clean (2 sessions):
bazel run //examples/deep-search:deep_search -- "Who won the last Formula 1 race?"

# Aborts at exit (4 sessions):
bazel run //examples/deep-search:deep_search -- \
  "Compare the camera quality, battery life, and performance of the latest \
  flagship smartphones from Apple, Samsung and Google, and recommend the \
  best overall choice."
```

Capture the stack with `coredumpctl list` + `coredumpctl dump <PID>` (gdb is
not installed on this host; the coredump message itself already carries the
symbolized stack).

## 8. Suggested fix directions (engine side, upstream)

1. Audit `session_basic`'s destroy path against the compiled-model
   executor's `TensorBuffer` registry: every buffer freed by a session must
   be removed from the registry in the same call.
2. Add an ASAN engine build for the multi-session lifecycle test; note the
   framework binary currently cannot start under `--config=asan` because
   protobuf's static descriptor registration crashes before `main` (separate
   environment issue, worth a dedicated look).
3. Add an upstream regression test: create 4 conversations, run one turn
   each, destroy them, destroy the engine — must exit cleanly.

## 9. Related artifacts

- Fixes on `feat/parallel-tool-dispatch`: `34fba14` (quote tokens),
  `67605d4` (serialization slot), `968de8d` (docs).
- Run logs: `/tmp/opencode/local_final.log` (crash),
  `/tmp/opencode/local_serialized.log` (clean single-delegation run).
- Design context: `docs/superpowers/specs/2026-08-30-parallel-tool-dispatch-design.md`
  (§3.3 validation point).
