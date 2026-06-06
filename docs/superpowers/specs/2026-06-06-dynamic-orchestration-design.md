# Dynamic Workflow Orchestration — Design Spec

**Date:** 2026-06-06
**Status:** Approved design, ready for implementation planning.
**Anchors:** Builds on agentflow P1–P9. Adds a JSON-driven workflow layer
on top of the existing C++ engine. Closes the "hot-pushable, multi-tenant,
sub-agent-aware" production gap.

## 1. Goal and scope

Add a runtime workflow orchestration layer that lets authors:

1. **Define workflows in JSON** instead of imperative C++ / Kotlin code.
2. **Configure data flow between agents** via templates rather than hard-coded
   field names.
3. **Spawn sub-agents during execution** with clean, isolated context, with
   the parent LLM choosing both *which* sub-agent and *what* goal at call
   time.

Three production scenarios drive the design:

- **Hot-update / remote distribution.** Apps ship with the agentflow
  runtime; new workflows arrive as signed JSON from a cloud endpoint or
  cache, with no app redeploy.
- **Multi-workflow apps.** A single host process serves many named
  workflows (`customer_support`, `translation`, `code_assistant`) and
  switches between them per request.
- **Sub-agent task isolation.** A main agent farms out focused sub-tasks
  (token-budgeted RAG queries, structured analyses) to fresh sub-agents
  whose conversation history doesn't pollute the parent.

The engine lives in **C++**; Kotlin, Python, and other clients hand it
JSON via the P9 JNI surface. The existing imperative `GraphBuilder` API
keeps working unchanged for code-defined graphs.

## 2. Architecture overview

A new module `agentflow/workflow/` sits **on top of** the existing
`core` / `nodes` / `tools` layers:

```
agentflow/workflow/
├── workflow_spec.proto        ← JSON-compatible schema
├── workflow_registry.{h,cc}   ← named workflow lookup + hot-update
├── workflow.{h,cc}            ← loaded workflow: schema + materialized Graph + roster
├── workflow_loader.{h,cc}     ← JSON → WorkflowSpec → Workflow (validation here)
├── delegate_tool.{h,cc}       ← built-in tool exposing the sub-agent roster
├── sub_agent_runtime.{h,cc}   ← per-call sub-agent lifecycle
├── template_engine.{h,cc}     ← `{{path.to.value}}` substitution
├── json_path.{h,cc}           ← JSONPath subset for output_extract
└── workflow_state.{h,cc}      ← dynamic-JSON State backing
```

The runtime pipeline:

```
JSON file  →  WorkflowLoader::Load()  →  Workflow (validated, materialized)
                                            │
WorkflowRegistry::Register(name, version, Workflow)
                                            │
caller asks: registry.GetLatest("chat")  →  Workflow
                                            │
caller: Runner runs the Workflow's main graph as normal
                                            │
mid-run: parent agent's LLM calls delegate(agent=X, goal=Y)
                                            │
delegate_tool.Invoke()  →  sub_agent_runtime spawns fresh
                            LiteRtLmConversation for agent X
                            with templated input, runs to completion,
                            extracts output via JSONPath,
                            returns string to parent's tool result
```

**Isolation contract.** Each `delegate(...)` call constructs a brand-new
`LiteRtLmConversation` (no shared history with parent), with its OWN tool
registry slice (only the tools that agent X declares), its OWN
AgentNodeConfig (own system prompt, own max_iter). Sub-agent termination
releases the conversation; nothing leaks back into the parent except the
return string + emitted trace events.

## 3. `WorkflowSpec` JSON schema

Authors write JSON like this:

```json
{
  "schema_version": 1,
  "name": "customer_support",
  "version": "2026-06-06.3",

  "state": {
    "kind": "dynamic_json",
    "fields": {
      "user_query":      {"type": "string"},
      "assistant_reply": {"type": "string"}
    }
  },

  "agents": {
    "planner": {
      "system_prompt": "Break the user request into <=3 sub-questions.",
      "model": {"max_output_tokens": 256, "constrained_tool_calls": false},
      "tools": []
    },
    "researcher": {
      "system_prompt": "Answer one focused sub-question using tools.",
      "model": {"max_output_tokens": 512, "constrained_tool_calls": true},
      "tools": ["web_search", "rag"]
    },
    "support": {
      "system_prompt": "You are a support agent. Use `delegate` to call planner or researcher; combine results into a final reply.",
      "model": {"max_output_tokens": 1024, "constrained_tool_calls": true},
      "tools": ["get_order_status"],
      "delegates": {
        "agents": ["planner", "researcher"],
        "max_depth": 2,
        "parallel": true,
        "input_template": {
          "user_query": "{{tool.goal}}",
          "_parent_query": "{{state.user_query}}"
        },
        "output_extract": "$.assistant_reply"
      }
    }
  },

  "main": "support",

  "signing": {
    "algo": "HMAC-SHA256",
    "key_id": "prod-2026-q2",
    "signature": "<base64 over the canonical JSON sans this field>"
  }
}
```

**Field semantics:**

- `schema_version` — bumped only on incompatible JSON-shape changes.
  Loader rejects if `> kCurrentWorkflowSchemaVersion`.
- `name` + `version` — together form the registry key. Hot-update means
  push a new `version` string and tell the registry to reload.
- `state.kind` — one of `dynamic_json`, `proto`, `proto_dynamic` (see §5).
- `state.fields[X].type` (tier 1 only) — one of `string`, `integer`,
  `number`, `boolean`, `array`, `object`. Optional `default` per field;
  if omitted, defaults are `""` / `0` / `0.0` / `false` / `[]` / `{}`.
- `agents` — the roster. Each entry materializes into an `AgentNodeConfig`
  template. Tool names reference the host-provided `ToolRegistry`; the
  workflow does NOT define tool implementations (host owns those —
  preserves security). The framework auto-registers a built-in `delegate`
  tool into the agent's tool slice when (and only when) its JSON has a
  `delegates` block; the load step rejects any author tool whose name
  collides with `delegate`.
- `agents[].delegates` — present only on agents that may call `delegate(...)`.
  Lists which roster members are reachable, recursion limit, whether
  multi-tool-call-per-turn fans out concurrently, and the templates for
  goal/input/output routing.
- `main` — the name of the agent that runs the parent ReAct loop. Must
  exist in `agents`.
- `signing` — optional. When present, the loader rebuilds the canonical
  JSON (sorted keys, signing field removed), HMACs it, compares. Mismatch
  → rejection.

**Two notable choices baked in:**

1. **Workflows don't define tool implementations.** The host process
   pre-registers `ToolRegistry` entries; the workflow lists tool names.
   A remote-pushed JSON can never inject code, only re-wire references.
2. **`delegates` is a per-agent property, not a graph-level property.**
   Multiple agents can have their own roster. Recursion is allowed but
   bounded by each agent's `max_depth`.

## 4. WorkflowRegistry + hot-update

A thin process-singleton (or app-owned) registry. Keyed by `name`; under
each name, an ordered map of `version` strings → `shared_ptr<Workflow>`.
Multiple versions of the same name can coexist while in-flight Runs hold
them.

```cpp
class WorkflowRegistry {
 public:
  std::shared_ptr<Workflow> Register(std::shared_ptr<Workflow> wf);
  std::shared_ptr<Workflow> GetLatest(std::string_view name) const;
  std::shared_ptr<Workflow> Get(std::string_view name,
                                std::string_view version) const;
  bool Unregister(std::string_view name, std::string_view version);

  struct Entry { std::string name, version; absl::Time registered_at; };
  std::vector<Entry> List() const;
};
```

**Hot-update semantics:**

1. **Next-Run swap only.** A new `Register` does NOT affect any Run
   already in flight. New version takes effect on the *next* `GetLatest`.
   Mirrors P7's D11 ("explicit > implicit; no silent mid-flight swap").
2. **Concurrent versions.** Old version's `shared_ptr` is retained until
   the last Runner releases it (RAII).
3. **No persistence in-engine.** App persists JSON files and re-runs
   `Register(WorkflowLoader::Load(json))` at startup. Crash-safety surface
   stays explicit.
4. **Validation at `Register`, not `Get`.** `Get` is cheap and never
   fails for in-registry entries.
5. **Multi-workflow scenarios just work** via the (name, version) key.

**Trace coupling.** `Register` and `Unregister` emit framework-level
events (`WORKFLOW_REGISTERED{name, version, signed}`, `WORKFLOW_UNREGISTERED`)
via the process EventEmitter if configured. Useful for audit logging in
the remote-distribution scenario.

## 5. Sub-agent execution

### 5.1 The `delegate` tool

When a `Workflow` materializes, for every agent whose JSON has
`delegates`, the framework registers a built-in tool named `delegate`
into **that agent's tool slice** with the schema:

```json
{
  "name": "delegate",
  "description": "Hand a sub-task to another agent. The chosen agent runs with clean context and returns a result string.",
  "params_json_schema": {
    "type": "object",
    "properties": {
      "agent": {"type": "string", "enum": ["planner", "researcher"]},
      "goal":  {"type": "string"}
    },
    "required": ["agent", "goal"]
  }
}
```

The `enum` is materialized from `delegates.agents`. When the parent agent
has `constrained_tool_calls = true` (P8), LLGuidance's Lark grammar makes
it literally impossible for the LLM to emit a non-roster agent name.

### 5.2 Isolation per invocation

When the parent LLM emits `delegate(agent="X", goal="...")`:

1. A brand-new `LiteRtLmConversation` is constructed with **agent X's**
   system_prompt + **agent X's** tools_json. Parent's conversation untouched.
2. Agent X's tools are exactly what X declared in its own `tools` array
   PLUS an auto-registered `delegate` if X's JSON also has a `delegates`
   block (this is what enables nested delegation). The parent's tool slice
   is NEVER visible to X.
3. The first message X sees is built from `input_template`. By default
   `{"role":"user","content":[{"type":"text","text":"{{tool.goal}}"}]}`.
4. X runs a complete ReAct loop bounded by X's `max_iter`.
5. Result handed back is the string extracted via `output_extract`
   (default `$.assistant_reply`).

**The boundary is hard:** parent ↔ sub-agent communication = (a) goal
string in, (b) result string out. Cross-contamination requires explicit
`input_template` action.

### 5.3 Parallel delegation

If the LLM emits multiple `delegate` calls in one turn:

- `parallel: true` → each call spawned in `asio::co_spawn`, all awaited,
  results stitched back in original tool_calls order. Mirrors
  `TeamNode::RunParallelGather`.
- `parallel: false` → sequential in LLM order.

Each call carries an `invocation_id` for trace grouping.

### 5.4 Recursion + depth limit

Sub-agents can themselves call `delegate` if their JSON declares
`delegates`. Each invocation carries a `SubAgentContext { depth,
root_invocation_id, parent_cancel }`:

- `depth = 0` at the workflow's `main` agent.
- Each delegate-call increments depth.
- `depth >= max_depth` → tool result `{"error": "max_depth_exceeded"}`.
- Static acyclicity check at load catches obvious cycles.

### 5.5 Cancellation propagation

Parent's `CancelToken` threads through every sub-agent's
`SubAgentContext.parent_cancel`. Cancel fires → in-flight sub-agents
observe the token → each delegate returns `{"error": "cancelled"}` →
parent's ReAct loop exits via existing P6 path. No new cancel API.

### 5.6 Error handling

Every sub-agent failure becomes a JSON-stringified tool result the LLM
sees and reasons about — none escape as C++ exceptions. Detailed
taxonomy in §10.

### 5.7 Checkpoint interaction

Sub-agent calls are transactional. Parent's `NODE_END` only fires after
all delegations resolve. Sub-agent intermediate state is never preserved.
On resume, the parent agent re-runs from scratch; delegate calls
re-issue. Sub-agent tools must therefore be idempotent if you want clean
resume — same rule as any P7-resumed graph.

## 6. State model (three tiers)

```json
"state": { "kind": "dynamic_json", "fields": {...} }                    // tier 1
"state": { "kind": "proto",         "message_type": "myapp.ChatState" } // tier 2
"state": { "kind": "proto_dynamic", "message_type": "myapp.ChatState",
           "descriptor_set_path": "./state.desc" }                       // tier 3
```

| Tier | Author ships | When to pick it |
|---|---|---|
| `dynamic_json` | Just the workflow JSON | Quickest. Remote-distributed configs with no build step. |
| `proto` | `.proto` compiled into the host binary | Host already uses typed messages. |
| `proto_dynamic` | `.proto` + `.desc` shipped alongside JSON | Protobuf tooling without host having compile-time knowledge of the type. |

### 6.1 `State` becomes a tagged union

```cpp
class State {
 public:
  template <class T> static State From(T&& msg);                    // tier 2
  static State FromDynamicProto(                                     // tier 3
      std::shared_ptr<google::protobuf::DescriptorPool> pool,
      std::string_view message_type);
  static State FromJson(const nlohmann::ordered_json& fields_decl); // tier 1

  enum class Kind { Proto, ProtoDynamic, Json };
  Kind kind() const noexcept;

  std::string SerializeAsString() const;
  bool ParseFromString(std::string_view data);

 private:
  // Tier 3: keepalive for DescriptorPool. MUST outlive every Message
  // instance constructed via DynamicMessageFactory(pool.get()).
  std::shared_ptr<google::protobuf::DescriptorPool> pool_;
  std::variant<std::unique_ptr<google::protobuf::Message>,
               std::unique_ptr<nlohmann::ordered_json>> backing_;
};
```

### 6.2 Field access stays uniform

Canonical helpers in `agentflow/core/state.h`:

```cpp
std::string ReadStringField(const State& s, std::string_view path);
void WriteStringField(State& s, std::string_view path, std::string_view value);
const nlohmann::ordered_json* AsJson(const State& s);
nlohmann::ordered_json* MutableJson(State& s);
```

- Tiers 2 + 3 → protobuf reflection. The fact that one tier's descriptors
  live in `generated_pool()` and the other's in a custom pool is invisible
  at this layer.
- Tier 1 → JSONPath-style dotted access (`user_query`, `nested.answer`,
  `messages[0].content`). Nested writes auto-create intermediate objects.

**Every existing node** (`AgentNode`, `RouterNode`, etc.) works on all
three tiers via these helpers, no per-node refactor.

### 6.3 Per-workflow pool isolation

Each tier-3 `Workflow` owns its own `DescriptorPool`. Lookup priority:
workflow pool first, then `generated_pool()`. Two workflows can both
declare `myapp.ChatState` with different fields without cross-contamination.

### 6.4 Schema declaration drives template + write-time validation

- Tier 1: declared `fields` are the contract. Template `{{state.X}}`
  validated against declared fields at load time. Undeclared writes are
  scratchpad (allowed but unvalidated).
- Tiers 2/3: descriptor IS the contract. Template validation uses
  `Descriptor::FindFieldByName`. Type-mismatched writes fail at runtime
  via existing reflection behavior. Writes to undeclared fields are
  silent no-ops (`FindFieldByName` returns null; `WriteStringField`
  bails) — this asymmetry with tier 1's scratchpad behavior is by design:
  protobuf states are strictly typed, JSON states are intentionally
  open-ended.

### 6.5 Checkpoint compatibility

- Tier 1: `state_type = "agentflow.JsonState"` sentinel. `state_bytes` is
  UTF-8 JSON.
- Tier 2/3: `state_type = "myapp.ChatState"`. Tier 3 resume requires the
  matching workflow to be re-registered (carries the pool).

## 7. Template engine

Pure substitution. No expressions, no code execution.

### 7.1 Syntax

`{{path}}` substitution. Dotted (`state.user_query`,
`parent.state.user_query`, `tool.goal`). Whitespace inside braces
tolerated. Literal `{{` produced by `\{{`.

### 7.2 Evaluation contexts

| Template location | When | Context available |
|---|---|---|
| `agents[].system_prompt` | Once at load | `workflow.name`, `workflow.version` only |
| `agents[].delegates.goal_template` | Each delegate call | `tool.*`, `state.*`, `parent.*`, `workflow.*`, `now.*` |
| `agents[].delegates.input_template` (each value) | Each delegate call | Same as goal_template |
| `agents[].delegates.output_extract` | Each delegate response | JSONPath, not template — see §7.5 |

**Path namespaces:**

- `state.X` — `ReadField(state, "X")`. Tier-aware.
- `tool.X` — LLM-supplied tool-call argument.
- `parent.state.X` — parent's state, read-only. Only inside delegate templates.
- `workflow.name`, `workflow.version` — workflow metadata.
- `now.iso`, `now.unix_micros`, `now.unix_seconds` — wall-clock.

Anything else is a load-time error.

### 7.3 Type coercion

- **Pure substitution** (`{{state.tags}}` with nothing else): result
  preserves underlying type — useful for injecting structured input.
- **String interpolation** (`"Answer for: {{state.user_query}}"`): each
  `{{...}}` JSON-serialized then concatenated.

### 7.4 Load-time validation

For each template string, loader:
1. Parses `{{...}}` segments; checks brace balance.
2. Resolves each path head against declared schemas of available contexts.
3. Reports unresolved paths as `WorkflowLoader::Error` with location.

### 7.5 `output_extract` — JSONPath subset

Separate, smaller language. Operates on sub-agent response JSON:

- `$` — root
- `.field` — nested object key
- `[N]` — array index

If extraction fails, fall back to the raw response JSON as string + emit
`SUB_AGENT_EXTRACT_FAILED`.

### 7.6 Sandboxing posture

Pure substitution. No file I/O, no env vars, no arbitrary expressions, no
shell-out. A remote-distributed workflow cannot do anything dangerous
through templates beyond reading what's explicitly allowed.

## 8. Trace events

P5 trace channel grows new kinds for workflow lifecycle and sub-agent
execution. Reuses existing `EventEmitter` / `proto::TraceEvent` envelope.

### 8.1 New `TraceEvent.kind` values

```proto
enum Kind {
  // ... existing kinds
  SUB_AGENT_START          = 9;
  SUB_AGENT_END            = 10;
  SUB_AGENT_EXTRACT_FAILED = 11;
  WORKFLOW_REGISTERED      = 12;
  WORKFLOW_UNREGISTERED    = 13;
}

message SubAgentStartPayload {
  string parent_agent       = 1;
  string child_agent        = 2;
  string invocation_id      = 3;
  string root_invocation_id = 4;
  uint32 depth              = 5;
  string goal               = 6;
}

message SubAgentEndPayload {
  string invocation_id = 1;
  uint32 depth         = 2;
  bool   success       = 3;
  string error_kind    = 4;
  uint32 output_chars  = 5;
}

message SubAgentExtractFailedPayload {
  string invocation_id = 1;
  string json_path     = 2;
}

message WorkflowRegisteredPayload {
  string name    = 1;
  string version = 2;
  bool   signed_ = 3;
  string key_id  = 4;
}

message WorkflowUnregisteredPayload {
  string name    = 1;
  string version = 2;
}
```

Additive proto change: existing consumers continue to parse old events;
new consumers see the richer surface.

### 8.2 Dual view (tools + agent tree)

Existing `TOOL_CALL` / `TOOL_RETURN` events still fire for `delegate`.
Combined with the new `SUB_AGENT_*` events:

```
NODE_START          {node_id: "support"}
  TOOL_CALL         {tool_name: "delegate"}
    SUB_AGENT_START {parent: "support", child: "researcher", invocation_id: "u1"}
      ...
    SUB_AGENT_END   {invocation_id: "u1", success: true}
  TOOL_RETURN       {tool_name: "delegate"}
NODE_END
```

Observability tools choose: tool-call stream (for tool dashboards) or
agent-tree view (Langfuse / Phoenix-style nested traces).

### 8.3 Remote-distribution audit

Together with `WorkflowRegisteredPayload.{key_id, signed_}`, trace events
form a sufficient audit trail for hot-pushed configs. A `JsonlEventEmitter`
writing to a tamper-resistant log gives the security team "what config
ran, when, who signed it, what sub-agents got spawned." No new
persistence machinery.

## 9. Validation, JSON parsing, signing

### 9.1 Parse phase

nlohmann JSON parse → `proto::WorkflowSpec`. Two failure modes:
`kParse` (bad JSON), `kStructural` (parses but doesn't match spec).

### 9.2 Schema version negotiation

```cpp
inline constexpr uint32_t kCurrentWorkflowSchemaVersion = 1;
```

- Equal → load.
- Older → run pure-data migration table.
- Newer → `kSchemaTooNew`.

### 9.3 Validation pipeline

1. **Resource limits** (cheap, fail fast):
   - Total JSON size ≤ 256 KB
   - `agents` count ≤ 32
   - Per-agent `tools` count ≤ 64
   - Per-agent `delegates.agents` count ≤ 16
   - Per-agent `max_depth` ≤ 8 (hard ceiling)
   - Template strings ≤ 4 KB each
2. **Reference resolution:** every tool name resolves in host
   ToolRegistry; every `delegates.agents` name resolves in this
   workflow's agents; `main` exists.
3. **State schema:** tier-specific descriptor / fields resolution.
4. **Template validation** (§7.4).
5. **Static recursion check:** delegation graph is acyclic.
6. **Signature verification** (§9.4), if `signing` present.

Errors accumulate into a `LoadReport`; the loader does NOT short-circuit
on the first error.

### 9.4 Signing

**Algorithm: HMAC-SHA256.** No RSA / ECDSA in v1. Key rotation via
`key_id`.

**Canonical form for HMAC:**

1. Remove the entire `signing` block.
2. Recursively sort all object keys lexicographically.
3. Serialize with no whitespace, no trailing commas, UTF-8, no BOM.

```cpp
class KeyResolver {
 public:
  virtual ~KeyResolver() = default;
  virtual absl::StatusOr<std::string> Resolve(absl::string_view key_id) = 0;
};

struct LoaderOptions {
  size_t max_json_bytes = 256 * 1024;
  KeyResolver* key_resolver = nullptr;
  bool require_signed = false;
  EventEmitter* trace = nullptr;
};
```

### 9.5 Threat model

**Defended against:**
- Code injection via templates (pure substitution).
- Non-existent tool references (loader catches).
- Infinite recursion (static + runtime depth limits).
- Memory exhaustion via huge declarations (resource limits).
- Cross-workflow impersonation (name+version unique).
- Modified-in-transit JSON (HMAC).

**Out of scope (host's responsibility):**
- Host tool implementations themselves.
- Logically wrong but correctly-signed workflows.
- Side-channel attacks.

### 9.6 Loader API

```cpp
class WorkflowLoader {
 public:
  static std::shared_ptr<Workflow> Load(
      std::string_view json_text,
      const ToolRegistry& host_tools,
      const LoaderOptions& opts = {});

  static std::shared_ptr<Workflow> LoadFromFile(
      const std::string& path,
      const ToolRegistry& host_tools,
      const LoaderOptions& opts = {});
};
```

`Load` is the only path into the registry.

## 10. Error handling (unified taxonomy)

### 10.1 Three audiences

| Audience | When | Mechanism |
|---|---|---|
| **Author** (deploy-time) | `WorkflowLoader::Load` | Throws `AgentflowError` with `LoadReport` |
| **LLM** (runtime, inside agent loop) | Sub-agent fails, output extract misses, max_depth, host tool throws | Tool result is `{"error": "<kind>", ...}` JSON |
| **Host** (runtime, graph cannot recover) | Parent's `LiteRtLmConversation::Create` fails, signed-key resolver throws, OOM | `Runner::Run` rethrows |

**Hard rule:** sub-agent-level failures do NOT escape to the host.

### 10.2 Canonical error kinds

Author-visible: `parse_error`, `structural_error`, `schema_too_new`,
`schema_upgrade_missing`, `signature_invalid`, `signature_required`,
`unknown_tool`, `unknown_agent`, `cyclic_delegation`, `bad_template`,
`bad_state_schema`, `resource_limit_exceeded`.

LLM-visible: `engine_create_failed`, `sub_agent_threw`,
`max_iter_exceeded`, `max_depth_exceeded`, `extract_failed`, `cancelled`,
`tool_threw`.

Host-visible: `engine_oom`, `runtime_panic`.

These names appear verbatim in `SubAgentEndPayload.error_kind`.

### 10.3 Resource-exhaustion specifics

- **Load-time caps** → `resource_limit_exceeded` to author.
- **Runtime budgets** (`max_iter`, `max_depth`) → LLM-visible tool result.
- **Cancellation** → LLM sees `cancelled` tool result AND host's
  `Runner::Run` returns most-recent terminal state.

No "max failed delegate calls" counter — the parent LLM is the supervisor
and decides when to give up. Baking in a hard counter would create
surprise terminations the LLM couldn't reason about.

### 10.4 Schema drift on resume

P7's `Runner::Resume` enforces `state_type` match. New rules for
three-tier model:

- Resume requires the same Workflow registered (or compatible version).
- Mismatched `state.kind` → `AgentflowError`.
- Tier 3 with missing descriptor → `AgentflowError("descriptor missing on resume")`.

All surface to host, not LLM.

### 10.5 What hosts must handle

- `try`/`catch` around `WorkflowLoader::Load` (hot-update endpoints).
- `try`/`catch` around `Runner::Run` (runtime panics).
- Error sink for `LoadReport.errors`.
- Trace handling for `SUB_AGENT_END{error_kind != ""}`.

## 11. Testing strategy

Each module gets focused unit tests; full-stack tests gated on `MODEL_PATH`.

| Layer | Test kind | Examples |
|---|---|---|
| `workflow_loader` | Hermetic | round-trip parse / serialize; every error kind; signing+resolver; migration table |
| `template_engine` | Hermetic | path resolution, type coercion, validation failures, parent/state/tool contexts |
| `json_path` | Hermetic | every grammar form, miss fall-through |
| `workflow_state` (tier 1) | Hermetic | JSON path access, nested writes, undeclared scratchpad |
| `state` tier 3 | Hermetic | DescriptorPool lifetime, two workflows with same message_type |
| `workflow_registry` | Hermetic | concurrent versions, RAII keepalive, multi-name lookup |
| `sub_agent_runtime` | Hermetic | isolation (parent tools NOT visible to child), depth tracking, parallel fan-out via mock conversation |
| `delegate_tool` | Hermetic | enum constraint enforced, error kinds surfaced as tool results |
| Full stack | MODEL_PATH-gated | run a tier-1 workflow with two sub-agents end-to-end; assert sub-agent isolation via captured Conversation snapshots |

Trace assertions piggyback on `CallbackEventEmitter` from P5 — every new
event kind gets at least one test that captures it.

## 12. Out of scope (deferred to future phases)

- Conditionals / loops / filters in templates.
- Per-token streaming from sub-agents (sub-agents are sync in v1).
- Cross-sub-agent shared state.
- Per-sub-agent timeouts (use parent cancel + max_iter).
- Sub-agent-level checkpointing (checkpoint at parent boundaries only).
- Runtime parsing of `.proto` source (would need libprotoc).
- Tier-1 dynamic field schema enforcement beyond type-check (no enum/min/max).
- Asymmetric signatures (RSA/ECDSA). Reserved `signing.algo` field for
  future addition.
- Cross-major-version schema migrations (require explicit rewrite tools).
- Per-workflow CPU / memory quotas (host-level concern).
- Distributed registry (each process has its own; cluster coordination is
  the host's job).

## 13. Implementation surface estimate

```
proto/workflow_spec.proto                       ~150 lines
proto/trace_event.proto                         +5 enum vals + 5 payloads
agentflow/core/state.{h,cc}                     ~200 LOC (3-tier variant + helpers)
agentflow/workflow/workflow.{h,cc}              ~150 LOC
agentflow/workflow/workflow_registry.{h,cc}     ~150 LOC
agentflow/workflow/workflow_loader.{h,cc}       ~600 LOC (validation pipeline)
agentflow/workflow/delegate_tool.{h,cc}         ~120 LOC
agentflow/workflow/sub_agent_runtime.{h,cc}     ~300 LOC
agentflow/workflow/template_engine.{h,cc}       ~180 LOC
agentflow/workflow/json_path.{h,cc}             ~80 LOC
agentflow/workflow/workflow_state.{h,cc}        ~150 LOC
tests/unit/workflow/*                           ~800 LOC across modules
tests/integration                               ~200 LOC
```

Roughly 3000 LOC of new C++ plus tests. No new third-party deps —
nlohmann + protobuf + abseil are already linked. The JNI layer (P9) gains
two new functions to bridge `WorkflowLoader` + `WorkflowRegistry`.

## 14. Spec compatibility with existing phases

| Phase | Interaction |
|---|---|
| P1 core | `Workflow.MaterializeGraph()` calls the existing `GraphBuilder`. No core changes. |
| P2 agent layer | Sub-agents instantiate `AgentNode` directly. The `delegates` block lives on the **WorkflowSpec agent definition** (not on `AgentNodeConfig` itself); the loader translates it into a `delegate`-tool registration on that agent's `ToolRegistry` slice. `AgentNodeConfig` gains no new fields. |
| P3 MCP | MCP tools register into `ToolRegistry` like any other — referenced by name from workflows. |
| P4 multi-agent | TeamNode coexists. Authors who want declarative multi-agent: TeamNode. Who want LLM-routed: workflows with `delegate`. |
| P5 trace | Strictly additive. New event kinds; existing sinks unchanged. |
| P6 conversation | Sub-agents use the same `LiteRtLmConversation::SendMessageSync`. |
| P7 checkpoint | Sub-agent calls transactional from parent's checkpoint POV. Three-tier state interacts via `state_type` discrimination. |
| P8 constrained decoding | `delegate` tool's enum is automatically constrained when parent agent has `constrained_tool_calls=true`. Free win. |
| P9 JNI MVP | `WorkflowLoader::Load` + `WorkflowRegistry::GetLatest` exposed through two new JNI functions; Kotlin DSL gains a `loadWorkflow(json)` helper. |

## 15. Open questions surfaced for implementation phase

- Whether `tier 3` descriptor sets should be cacheable (parse-once,
  many-workflows-share-pool). Likely yes if profiling shows hot path.
- Whether `signature.algo` should accept multiple values per workflow
  (key rollover overlap). Defer — single algo for v1.
- Whether the registry should expose a "compatible version range" query
  for Resume. Defer — explicit version match is cleaner.
