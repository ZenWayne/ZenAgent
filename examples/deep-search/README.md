# Deep-Search Example

A deep-search workflow on AgentFlow: a planner agent splits a question into
sub-questions, fans out one `delegate` call per sub-question **in a single
turn** (the dispatch loop runs them concurrently), then synthesizes a cited
answer from the gathered results. Each searcher uses `tavily_search` +
`tavily_extract` (server-side multi-URL fetch) for research.

## Configuration

Copy `env.example` to the repo-root `env.local` (gitignored), fill in real
values, then load it into the shell:

```bash
cp examples/deep-search/env.example env.local   # edit with real keys
set -a; source env.local; set +a
```

Credentials never enter `workflow.json` — only host code reads them from the
environment.

## Run — cloud mode (default)

```bash
set -a; source env.local; set +a
bazel run //examples/deep-search:deep_search -- "Who won the last Formula 1 race?"
```

## Run — local mode (wall-time comparison)

When `MODEL_PATH` is set, the identical workflow runs against the local
LiteRT-LM engine (gemma-4-E2B-it):

```bash
set -a; source env.local; set +a
MODEL_PATH=models/gemma-4-E2B-it.litertlm \
  bazel run //examples/deep-search:deep_search -- "Who won the last Formula 1 race?"
```

Both modes print `mode=` and `elapsed_ms=` at the end.

## Real-cloud e2e test

```bash
set -a; source env.local; set +a
bazel test //tests/integration:deep_search_e2e_test --test_output=all \
  --test_timeout=300
```

Skipped automatically when the credentials are unset (CI stays offline).
Asserts structure only: non-empty final answer, no error placeholders from
sub-agents, and — when the model fans out ≥2 delegate calls — every delegate
TOOL_CALL trace event precedes every TOOL_RETURN (proof of concurrent
dispatch; a sequential loop would interleave call/return pairs).

## Wall-time comparison

Measured 2026-08-30 on the same query ("Compare the camera quality, battery
life, and performance of the latest flagship smartphones…"):

| Mode | elapsed_ms | Result |
|---|---|---|
| cloud (deepseek-v4-flash via gateway) | **83,853** | Complete cited answer; 3 delegate calls fanned out concurrently (e2e assertion passed) |
| local gemma-4-E2B-it | **51,859** | Ran to completion but the final answer was EMPTY — the model's unconstrained tool-call formatting drifted, and the last turn produced an empty text response. |

**Local-mode conclusion (validation point from the spec):** gemma-4-E2B-it
does NOT reliably complete this workflow unconstrained — the planner's
tool-call turns misfire and the run ends with an empty answer. Additionally,
the LiteRT-LM engine decode threadpool runs with 1 worker thread
(`ThreadPool 'engine': Running up to 1 threads` in the run log), so even
concurrent sessions serialize inside the engine. The parallel dispatch
feature is therefore primarily a cloud-backend win; on-device deep-search
needs a constrained-decoding path or a model that holds its function-call
format unconstrained.
