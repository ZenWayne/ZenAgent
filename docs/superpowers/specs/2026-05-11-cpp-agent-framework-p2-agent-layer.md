# C++ Agent Framework — P2: Agent Layer

**Date:** 2026-05-11
**Status:** Draft
**Project:** ZenAgent (agentflow)

## 1. Overview

P2 adds the **agent layer** on top of P1's core graph runner. Users can define agents that use LLM inference and tools, connected via the same graph/runner infrastructure already built.

**Scope:**
- Migrate existing P1 CMake build to Bazel (LiteRT-LM already uses Bazel natively)
- `agentflow/inference/` — LiteRT-LM C API wrapper (async asio awaitable-based)
- `agentflow/tools/` — Tool, NativeFnTool, ToolRegistry (no MCP in P2)
- `agentflow/nodes/` — AgentNode (LLM + tool ReAct loop, single graph node)
- Stub engine for testing without real model
- Integration demo (AgentNode + ToolRegistry in a graph)

**Explicitly deferred to P3+:**
- MCP tool adapter (`McpToolAdapter`, `McpClientPool`)
- TeamNode (dynamic multi-agent dispatch)
- Checkpoint / persistence
- JNI + Kotlin DSL
- Trace emitter implementations (Perfetto/OTel)

## 2. Bazel Migration

### 2.1 Motivation

LiteRT-LM uses Bazel natively with full BUILD file coverage at every layer (`c/`, `runtime/`, etc.).
P1 used CMake, but maintaining two build systems creates friction for every P2+ change.
Full migration to Bazel eliminates the CMake ↔ Bazel impedance mismatch.

### 2.2 Strategy

- Write `MODULE.bazel` (Bzlmod) at project root declaring deps:
  - `asio` (standalone, header-only — `http_archive` or local copy)
  - `protobuf` (v25.3)
  - `abseil-cpp` (20240722)
  - `googletest` (v1.14)
  - `bazel_skylib`
  - `rules_proto` / `rules_cc`
- Write `BUILD.bazel` per directory for all existing P1 code:
  - `proto/BUILD.bazel` — `proto_library` + `cc_proto_library`
  - `agentflow/core/BUILD.bazel` — `agentflow_core` cc_library
  - `tests/unit/core/BUILD.bazel` — cc_test targets (all 39 P1 tests)
  - `examples/core-stub-graph/BUILD.bazel` — cc_binary
- Reference LiteRT-LM via `local_path_override` or `local_repository`
- Keep existing CMake files in tree (read-only) during transition; remove when Bazel covers everything
- `.bazelrc` configured for debug, ASan, and per-target instrumentation

### 2.3 Verification

```bash
bazel test //tests/unit/core/... --test_output=errors
# Expected: same 39 tests pass, same coverage as P1 CMake build
```

## 3. Inference Layer

**Directory:** `agentflow/inference/`

### 3.1 LiteRtLmEngine

Wraps `LiteRtLmEngine*` (opaque C pointer). Shared singleton — one engine per model file, multiple sessions can be created from it.

```cpp
class LiteRtLmEngine {
 public:
  struct Options {
    std::string model_path;
    std::string backend = "cpu";
    std::string cache_dir;
    int max_num_tokens = 4096;
  };

  static std::shared_ptr<LiteRtLmEngine> Create(Options opts);
  ~LiteRtLmEngine();

  // Not copyable, not movable (shared_ptr owns the engine pointer)
  LiteRtLmEngine(const LiteRtLmEngine&) = delete;
  LiteRtLmEngine& operator=(const LiteRtLmEngine&) = delete;

  // Create a session for inference. Thread-safe.
  std::unique_ptr<LiteRtLmSession> CreateSession(
      LiteRtLmSamplerParams sampler, int max_output_tokens);
};
```

### 3.2 LiteRtLmSession

Wraps `LiteRtLmSession*` — the core async streaming decoder.

Key design: LiteRT-LM's `generate_content_stream` fires a callback from a **background thread**.
We bridge this to asio via an internal `asio::experimental::channel`:

```
Background thread (LiteRT-LM internal)        asio executor
┌────────────────────────────────┐         ┌──────────────────────┐
│ callback(chunk) ───try_send──> channel ──co_await─> Session::NextTokenAsync()
│ callback(is_final) ─try_send──> channel              │
│ callback(error) ────try_send──> channel              │
└────────────────────────────────┘         └──────────────────────┘
```

```cpp
class LiteRtLmSession {
 public:
  ~LiteRtLmSession();

  // Non-blocking: starts prefill + decode on a LiteRT-LM internal thread.
  // Returns immediately; actual decoding proceeds in the background.
  // Takes the input text(s) to process.
  void Start(std::string_view input_text);

  // Returns the next decoded chunk via asio awaitable.
  // Empty string signals end of stream (is_final was received).
  // Throws on error (error_msg was non-NULL).
  asio::awaitable<std::string> NextTokenAsync();

  // Called from CancelToken::OnCancel — aborts the running session.
  void Abort();

  LiteRtLmSession(const LiteRtLmSession&) = delete;
  LiteRtLmSession& operator=(const LiteRtLmSession&) = delete;

 private:
  // Channel-based bridge: background callback → asio awaitable
  struct StreamState;
  std::unique_ptr<StreamState> stream_;
};
```

### 3.3 Bazel Target

```python
# agentflow/inference/BUILD.bazel
cc_library(
    name = "inference",
    srcs = ["litert_lm_engine.cc", "litert_lm_session.cc"],
    hdrs = ["litert_lm_engine.h", "litert_lm_session.h"],
    deps = [
        "//agentflow/core",
        "//LiteRT-LM/c:engine",     # depends on LiteRT-LM Bazel target
        "@asio",
    ],
)
```

## 4. Tool System

**Directory:** `agentflow/tools/`

### 4.1 Tool

Abstract interface:

```cpp
struct ToolSchema {
  std::string name;
  std::string description;
  std::string params_json_schema;   // JSON Schema for tool arguments
};

class Tool {
 public:
  virtual const ToolSchema& Schema() const = 0;
  virtual asio::awaitable<std::string> Invoke(
      std::string_view args_json,
      const CancelToken& cancel) = 0;
  virtual ~Tool() = default;
};
```

### 4.2 NativeFnTool

Wraps any `std::function` as a Tool:

```cpp
class NativeFnTool : public Tool {
 public:
  using Fn = std::function<asio::awaitable<std::string>(
      std::string_view, const CancelToken&)>;

  NativeFnTool(ToolSchema schema, Fn fn);
  const ToolSchema& Schema() const override;
  asio::awaitable<std::string> Invoke(
      std::string_view args_json, const CancelToken& cancel) override;
};
```

### 4.3 ToolRegistry

Name-based registry, thread-safe. Produces OpenAI-compatible tools JSON for LLM function calling.

```cpp
class ToolRegistry {
 public:
  void Register(std::shared_ptr<Tool> tool);
  asio::awaitable<std::string> Invoke(
      std::string_view name,
      std::string_view args_json,
      const CancelToken& cancel);

  // Example output:
  // [{"type":"function","function":{"name":"search","description":"...","parameters":{...}}}]
  std::string ExportToolsJson(std::span<const std::string> tool_names) const;
};
```

### 4.4 LLM Tool Call Format

ToolRegistry.ExportToolsJson produces the standard OpenAI-compatible format recognized by LiteRT-LM:

```json
[{
  "type": "function",
  "function": {
    "name": "search",
    "description": "Search the web",
    "parameters": {
      "type": "object",
      "properties": {
        "q": {"type": "string"}
      },
      "required": ["q"]
    }
  }
}]
```

LiteRT-LM returns tool calls as structured JSON in the decode output (same format as OpenAI: `{"tool_calls": [{"function": {"name": "...", "arguments": "..."}}]}`).

### 4.5 Bazel Target

```python
# agentflow/tools/BUILD.bazel
cc_library(
    name = "tools",
    srcs = ["native_fn_tool.cc", "tool_registry.cc"],
    hdrs = ["tool.h", "native_fn_tool.h", "tool_registry.h"],
    deps = [
        "//agentflow/core",
        "@asio",
        "@nlohmann_json",   # for JSON schema handling
    ],
)
```

## 5. AgentNode

**Directory:** `agentflow/nodes/`

### 5.1 Concept

AgentNode is a single graph node that encapsulates the ReAct loop: LLM reasoning → tool call → tool result → LLM reasoning → ... → final answer. The graph topology stays flat — AgentNode hides the loop internally.

```
                      ┌──────────────────────────────────┐
                      │          AgentNode                │
 State in ──────────> │                                  │ ──────────> State out
  .user_query         │  ┌─────┐   ┌──────┐   ┌──────┐  │   .assistant_reply
                      │  │ LLM │<──│ Tool │<──│ LLM  │  │
                      │  │     │──>│      │──>│      │  │
                      │  └─────┘   └──────┘   └──────┘  │
                      │   (iter 1)   (iter 2)   ...      │
                      └──────────────────────────────────┘
```

### 5.2 Config

```cpp
struct AgentNodeConfig {
  // LLM
  std::shared_ptr<LiteRtLmEngine> engine;
  std::string system_prompt;
  LiteRtLmSamplerParams sampler;
  int max_output_tokens = 512;
  bool stream_tokens = true;

  // Tools
  std::shared_ptr<ToolRegistry> tool_registry;

  // Loop control
  int max_iter = 8;

  // State field mapping (protobuf reflection)
  std::string input_field;          // read user input from this field
  std::string output_field;         // write assistant reply to this field
  std::string messages_field;       // if non-empty, append messages here (repeated Message field)
};
```

### 5.3 Run Loop

```
1. Read state[input_field] → get user input string
2. Build conversation context:
   - system_prompt (role=system)
   - messages from state[messages_field] (if multi-turn)
   - current user input (role=user)
   - tools_json from ToolRegistry::ExportToolsJson()
3. Create LiteRtLmSession from engine
4. Wire CancelToken::OnCancel → session.Abort()
5. session.Start(conversation_input)
6. Accumulate output tokens via session.NextTokenAsync()
7. If streaming: emit.EmitToken(node_id, token) per token
8. After stream completes:
   a. Parse output for tool_call JSON
   b. No tool_call → write full response to state[output_field], append to state[messages_field], done
   c. Tool call detected → ToolRegistry::Invoke(name, args, cancel)
   d. Append tool result to messages (role=tool)
   e. If iter < max_iter → go to step 5
   f. If max_iter exceeded → write "max iterations reached" to output_field
9. co_return state
```

### 5.4 Tool Call Parsing

LiteRT-LM returns tool calls in the decode output. Parser extracts:
- Function name (string)
- Arguments (JSON string)

The parser handles LiteRT-LM's specific JSON output format (similar to OpenAI's `tool_calls` format). If no tool_call is present, the raw output text is used as the assistant reply.

### 5.5 Error Handling

- LiteRT-LM stream error → set failure via `EmitNodeFailed` → exception propagates to runner
- Tool invocation failure → append error message as tool result (agent can recover)
- Cancel → `Abort()` called → session stops → `Run()` returns current state
- Max_iter exceeded → write fallback message, not an error

### 5.6 Bazel Target

```python
# agentflow/nodes/BUILD.bazel
cc_library(
    name = "nodes",
    srcs = ["agent_node.cc"],
    hdrs = ["agent_node.h"],
    deps = [
        "//agentflow/core",
        "//agentflow/inference",
        "//agentflow/tools",
        "@asio",
        "@nlohmann_json",
    ],
)
```

## 6. Project Structure (Final)

```
zen/
├── MODULE.bazel                    # Bazel bzlmod entry
├── BUILD.bazel                     # Root
├── .bazelrc
├── .bazelversion
├── proto/
│   ├── BUILD.bazel                 # proto_library + cc_proto_library
│   ├── errors.proto
│   ├── trace_event.proto
│   ├── test_messages.proto
│   └── [agent_messages.proto]      # NEW (optional — Message type for multi-turn)
├── agentflow/
│   ├── BUILD.bazel
│   ├── core/                       # P1 → BUILD.bazel added
│   │   ├── BUILD.bazel
│   │   ├── errors.h/cc
│   │   ├── cancel.h/cc
│   │   ├── event.h/cc
│   │   ├── state.h/cc
│   │   ├── edge.h
│   │   ├── node.h
│   │   ├── graph.h/cc
│   │   └── runner.h/cc
│   ├── inference/                  # NEW
│   │   ├── BUILD.bazel
│   │   ├── litert_lm_engine.h/cc
│   │   └── litert_lm_session.h/cc
│   ├── tools/                      # NEW
│   │   ├── BUILD.bazel
│   │   ├── tool.h
│   │   ├── native_fn_tool.h/cc
│   │   └── tool_registry.h/cc
│   └── nodes/                      # NEW
│       ├── BUILD.bazel
│       ├── agent_node.h
│       └── agent_node.cc
├── tests/
│   ├── BUILD.bazel
│   └── unit/
│       ├── BUILD.bazel
│       ├── core/                   # P1 tests → BUILD.bazel added
│       ├── inference/              # NEW
│       ├── tools/                  # NEW
│       └── nodes/                  # NEW
├── examples/
│   ├── BUILD.bazel
│   ├── core-stub-graph/           # P1 demo → BUILD.bazel added
│   └── agent-demo/                # NEW: AgentNode + tools in a graph
└── LiteRT-LM/                      # existing Bazel workspace (local_path_override)
```

## 7. Testing Plan

| Layer | File | Tests | What it covers |
|---|---|---|---|
| Core (P1) | `tests/unit/core/*` | 39 | State, Cancel, Event, Graph, Runner |
| Inference | `tests/unit/inference/*` | ~8 | Session streaming bridge, cancel→abort, multi-token sequence |
| Tools | `tests/unit/tools/*` | ~6 | NativeFnTool invoke, ToolRegistry register+invoke+error, JSON export format |
| Nodes | `tests/unit/nodes/*` | ~8 | AgentNode with real small model: no-tool, single-tool, multi-turn, max_iter, cancel |

**Total P2: ~22 new tests. Full suite: ~61 tests.**

### 7.1 Key Test Scenarios

- **AgentNode no-tool**: real model generates plain text → agent writes to output_field
- **AgentNode single tool**: real model returns tool_call → tool invoked → model generates final answer
- **AgentNode max_iter**: an agent configured to always call tools hits max_iter → fallback message
- **AgentNode cancel**: cancel mid-stream → Abort called → state returned promptly
- **Session bridge**: callback pushes multiple tokens → asio coroutine receives them in order
- **Session cancel**: session.Start() → Cancel() → session.Abort() called
- **ToolRegistry error**: invoke unknown tool → exception
- **ToolRegistry JSON**: registered tools → ExportToolsJson() produces correct JSON array

## 8. Acceptance Criteria

P2 is complete when:

1. Clean `bazel test //...` passes all P1 tests (39) and P2 tests (~22)
2. `agentflow/inference/` builds and links against LiteRT-LM's `c_engine` target
3. LiteRtLmSession correctly bridges a background-thread streaming callback to asio awaitables
4. `agentflow/tools/` provides NativeFnTool, ToolRegistry, and JSON export
5. AgentNode runs a ReAct loop with a real small model: input → LLM → tool_call → tool → LLM → output
6. Demo binary shows AgentNode + tools working end-to-end with a real model
7. Cancellation correctly aborts the LiteRT-LM session
8. All new code has UBSan/ASan clean builds
