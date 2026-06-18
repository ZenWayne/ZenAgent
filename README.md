# AgentFlow

**An on-device / embedded C++ agent framework — from inference to agents, full-stack.**

AgentFlow is a C++20 framework for building LLM agents that run **entirely on-device**: Android phones and embedded Linux ARM64 boards (Raspberry Pi, Jetson, …). It wraps Google's [LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM) inference engine and an MCP tool layer behind a DAG-based workflow engine, and ships a declarative **Kotlin DSL** for Android on top of the same C++ core.

> Status: active development. The C++ core, agent/tool layer, MCP client, multi-agent nodes, tracing, checkpointing, constrained decoding, and a JNI + Kotlin DSL MVP are implemented. iOS / desktop / server are explicitly out of scope for v1.

---

## Why

Most agent frameworks assume a cloud LLM and a server runtime. AgentFlow targets the opposite end: **local inference, no network required**, with token-level streaming and cooperative cancellation suitable for a chat UX on constrained hardware.

- **Inference backend** — LiteRT-LM (C ABI): prefill/decode, conversation management, abort hook.
- **Tools** — MCP client (stdio/SSE/WS/TCP) for ecosystem tools, plus in-process native tools (register a C++ or Kotlin lambda directly).
- **Workflow** — explicit DAG with `activation_group`s so cyclic edges don't deadlock (autogen #6711-style grouping).
- **Multi-agent** — an agent is just a graph node by default; complex dynamic scheduling is encapsulated in a `TeamNode`.

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  Kotlin DSL (Android)  —  workflow{} / agent{} / tool{}   │
│  Flow<TraceEvent> for streaming; zero business logic      │
└─────────────────────────┬────────────────────────────────┘
                          │  JNI (thin: protobuf marshal only)
                          ▼
┌──────────────────────────────────────────────────────────┐
│  C++ Public API (libagentflow) — source of truth          │
│  core/        Graph, Node, Edge, Runner, State,           │
│               Token/Stream/Cancellation                   │
│  nodes/       LlmNode, AgentNode, TeamNode,               │
│               RouterNode, AggregatorNode                  │
│  inference/   LiteRtLmEngine wrapper (session, streaming  │
│               decode, abort hook)                         │
│  tools/       ToolRegistry, McpToolAdapter, NativeFnTool  │
│  observability/  TraceEvent emitters (JSONL / callback)   │
│  persist/     Checkpoint hook (explicit resume only)      │
└────────────┬────────────────────────────┬────────────────┘
             ▼                             ▼
      ┌──────────────┐            ┌──────────────────┐
      │  LiteRT-LM   │            │  MCP (gopher-mcp)│
      └──────────────┘            └──────────────────┘
```

### Boundary rules

1. `core/` knows nothing about LiteRT-LM or MCP — it depends only on the `Node` interface. Swapping the inference engine or MCP implementation does not break the core API.
2. **The C++ library is the source of truth.** Kotlin is a thin DSL over JNI; it holds no business logic.
3. **Everything crossing a boundary is protobuf** — `State`, tool schema, trace events, graph spec, errors share `.proto` definitions (see [`proto/`](proto/)) for zero-copy JNI and version evolution.
4. Submodules (LiteRT-LM, MCP) are pinned and never modified; all adaptation lives in `inference/` and `tools/`.

## Repository layout

| Path | Contents |
|---|---|
| `agentflow/core/` | Graph, Node, Edge, Runner, State, cancellation, token channel |
| `agentflow/nodes/` | Agent / LLM / Team / Router / Aggregator nodes |
| `agentflow/inference/` | LiteRT-LM engine + session + conversation wrappers |
| `agentflow/tools/` | Tool registry, native-fn tools, MCP client + adapter |
| `agentflow/observability/` | Trace-event emitters (JSONL, callback, multi) |
| `proto/` | Shared protobuf schemas (state, workflow spec, MCP spec, trace, checkpoint) |
| `jni/` | `agentflow_jni.cc` — thin JNI bridge to the C++ runtime |
| `kotlin/` | Gradle project: `agentflow.dsl` DSL + `agentflow.jni.NativeBridge` |
| `examples/` | Runnable demos (agent, team, streaming, checkpoint, core graph) |
| `tests/` | Unit / integration / smoke tests |
| `docs/superpowers/` | Design specs and phase-by-phase implementation plans |
| `LiteRT-LM/` | Inference engine (vendored) |

## Building

The primary build system is **Bazel** (C++20). LiteRT-LM-aligned CMake files are also present.

```bash
# Build the whole framework
bazel build //agentflow/...

# Run the test suite
bazel test //tests/...

# Build and run a demo (needs a LiteRT-LM model)
bazel build //examples/agent-demo:agent_demo
MODEL_PATH=/path/to/model.litertlm \
  bazel-bin/examples/agent-demo/agent_demo
```

Sanitizer configs are available: `bazel test --config=asan //tests/...` (also `--config=ubsan`).

> **Note (proxy):** Bazel downloads may need JVM proxy args, e.g.
> `--host_jvm_args=-Dhttps.proxyHost=127.0.0.1 --host_jvm_args=-Dhttps.proxyPort=10808`.

## Examples

| Demo | What it shows |
|---|---|
| [`examples/agent-demo`](examples/agent-demo) | `AgentNode` + `ToolRegistry` in a graph, driven by a real model |
| [`examples/team-demo`](examples/team-demo) | Multi-agent scheduling via `TeamNode` |
| [`examples/streaming-demo`](examples/streaming-demo) | Token-level streaming + cooperative cancellation |
| [`examples/checkpoint-demo`](examples/checkpoint-demo) | Checkpoint hook (explicit resume) |
| [`examples/core-stub-graph`](examples/core-stub-graph) | Bare graph/runner with stub nodes (no model) |

### C++ sketch

```cpp
auto engine = af::LiteRtLmEngine::Create({.model_path = model_path});

auto registry = std::make_shared<af::ToolRegistry>();
registry->Register(std::make_shared<af::NativeFnTool>(
    af::ToolSchema{.name = "get_time", .description = "Get the current time",
                   .params_json_schema = "{}"},
    [](std::string_view, const af::CancelToken&) -> asio::awaitable<std::string> {
      auto tt = std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now());
      co_return std::ctime(&tt);
    }));

// build graph: entry --> agent --> sink, then drive it through the Runner
```

Nodes are coroutines (`asio::awaitable<State> Run(State, const CancelToken&, EventEmitter&)`), which makes streaming and cancellation first-class rather than bolted on.

### Kotlin DSL (Android / JVM)

```kotlin
val wf = workflow {
    agent("chat") {
        modelPath = "/path/to/model.litertlm"
        systemPrompt = "Reply briefly."
    }
}
val reply = wf.run("Say hello.")
```

Native build helper: `kotlin/scripts/build_native.sh` runs Bazel and copies `libagentflow_jni.so` into `kotlin/build/libs/native/`.

## Design decisions

A few load-bearing choices (full rationale in [`docs/superpowers/specs/`](docs/superpowers/specs/)):

- **C++20 with coroutines, exceptions, RTTI** — coroutines make token streaming and cooperative cancel tractable.
- **State is protobuf** — cross-JNI zero-copy and forward-compatible.
- **LLM session is node-private** — the KV cache is an inference detail and never leaks into workflow state.
- **DAG + activation groups** — topology is statically analyzable; cycles are handled by SCC grouping instead of round-robin.
- **Cooperative cancellation** — every node is cancel-aware; LLM nodes auto-wire `cancel` to LiteRT-LM's engine-level abort.
- **Checkpoint, no auto-resume** — resume is always explicit so stale state is never silently reused.

## Documentation

- Design specs: [`docs/superpowers/specs/`](docs/superpowers/specs/) — agent framework design, edge-agent service, dynamic orchestration.
- Implementation plans (phased P1–P9): [`docs/superpowers/plans/`](docs/superpowers/plans/) — core, agent layer, MCP, multi-agent, trace sink, conversation API, checkpoint, constrained decoding, JNI/Kotlin MVP.

## License

See the upstream licenses of the vendored components (LiteRT-LM and others). A project license has not yet been declared.
