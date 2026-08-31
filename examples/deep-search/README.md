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
does NOT complete this workflow unconstrained — but the root cause is not
threading (the engine queues sessions serially, which is only a speed cost).
Diagnosed via trace events: in the unconstrained path the model emits its
`open_quote` special token `<|"|>` literally inside tool-call arguments, and
the conversation layer does not decode it back to quotes. The delegate
`agent` argument therefore arrives as `<|"|>searcher<|"|>` and every
delegation fails with `{"error":"unknown_agent"}`, leaving the planner to
search on its own (with token-polluted queries) and finish with an empty
answer. Single-session `agent-demo` works only because `get_time` takes no
arguments — no quoted strings, no token leak. Any local agent with
string-typed tool arguments is affected; a proper fix belongs in the
LiteRT-LM conversation layer (decode gemma4 quote tokens on the
unconstrained path), or run local deep-search with `constrained_tool_calls`
(sequential delegation).
