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

| Mode | Command | elapsed_ms | Notes |
|---|---|---|---|
| cloud | (see above) | — | — |
| local gemma-4-E2B | (see above) | — | — |

Fill in after running; observe whether gemma-4-E2B-it emits multi-call turns
unconstrained (visible as multiple delegate TOOL_CALL events per turn).
