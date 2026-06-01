# C++ Agent Framework — P4: Multi-Agent Orchestration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete spec section 5 by landing `LlmNode` (single LLM call, no tools, no loop), the three orchestration primitives `TeamNode` (three policies), `RouterNode`, and `AggregatorNode`. After this plan, users can express a planner → researcher → writer flow as a single graph node and have the LLM (or pure code) decide which agent runs next, while keeping a single uniform `Node::Run` contract for the runner.

**Architecture:** All three nodes implement the existing `Node` interface — no runner changes. `TeamNode` does NOT recursively call `Runner`; it `co_await`s its members directly (per spec §5.3). The three `TeamNode` policies are encoded as a `std::variant`-style config so a single class handles all three. `RouterNode`'s decision shows up as a string field on `State` plus a thin pattern downstream nodes can use to self-skip. `AggregatorNode` exposes a user-supplied merge function and runs only after its incoming activation group satisfies.

**Tech stack:** C++20 (existing), Bazel 7.x (Bzlmod), asio 1.30, abseil 20260107, protobuf 31.1, nlohmann_json. No new external deps.

**Spec reference:** `docs/superpowers/specs/2026-04-27-cpp-agent-framework-design.md` §5.3, §5.4, §15 (Demo B acceptance).

---

## File Structure

```
zen/
├── proto/
│   └── (no changes — TeamNode config is C++ side; no JNI/proto in P4)
├── agentflow/
│   └── nodes/
│       ├── BUILD.bazel                      # MODIFY: add llm_node, team_node, router_node, aggregator_node
│       ├── agent_node.{h,cc}                # (existing P2 — ReAct loop, unchanged)
│       ├── llm_node.{h,cc}                  # NEW: single LLM call, no tools, no loop
│       ├── team_node.{h,cc}                 # NEW: 3-policy orchestrator
│       ├── router_node.{h,cc}               # NEW: state → NodeId decision
│       └── aggregator_node.{h,cc}           # NEW: fan-in merge
├── examples/
│   └── team-demo/                           # NEW: 2-agent team without a real model (stubs)
│       ├── BUILD.bazel
│       └── main.cc
└── tests/
    └── unit/nodes/
        ├── BUILD.bazel                      # MODIFY: add llm/team/router/aggregator tests
        ├── agent_node_test.cc               # (existing, DISABLED-needs-MODEL)
        ├── llm_node_test.cc                 # NEW: stub-LLM smoke; DISABLED real-LLM case
        ├── team_node_test.cc                # NEW: 3 policies × happy/edge cases
        ├── router_node_test.cc              # NEW
        └── aggregator_node_test.cc          # NEW
```

## Task Dependency

```
T1 (LlmNode — single LLM call)
   ↓
T2 (TeamNode skeleton + StateRouter)
   ↓
T3 (TeamNode ParallelGather) ──→ T5 (RouterNode + AggregatorNode)
   ↓                                              ↓
T4 (TeamNode LlmSelect, uses LlmNode) ────────→ T6 (unit tests)
                                                  ↓
                                         T7 (team-demo + wrap-up)
```

T1 lands LlmNode first so T4's LlmSelect moderator can use it directly. T2 lands the TeamNode skeleton with the simplest policy. T5 is independent of TeamNode and can run in parallel with T2–T4.

---

## Task 1: `LlmNode` — single LLM call, no tools

**Files:** Create `agentflow/nodes/llm_node.{h,cc}`; modify `agentflow/nodes/BUILD.bazel`.

**Context:** Spec §5.1 defines `LlmNode` as the minimal LLM primitive: it builds a conversation prompt (optionally including a `tools` array so the model can emit function-calling JSON), runs one streaming generation against `LiteRtLmEngine`, writes the accumulated reply to the State, and returns. The reply may contain a tool-call payload like `{"tool_calls":[{"function":{"name":...,"arguments":...}}]}` — `LlmNode` does **not** dispatch the call (that's `AgentNode`'s ReAct job, or a downstream tool node's); it just writes the raw reply to `output_field` for whoever's next in the graph to interpret. And it **does not** loop. Useful as (a) a building block for users who don't need ReAct, (b) the moderator inside `TeamNode::LlmSelect` (T4), (c) the cheapest possible "generate one structured response from this state, tools optional" primitive.

`LlmNode` reuses the existing `LiteRtLmSession` from P2; its `Run` body is `AgentNode::Run` minus the outer `for (iter < max_iter)` loop and the `HandleToolCall` dispatch branch — but it keeps the tools-in-prompt path so the LLM can produce structured tool-call output.

- [ ] **Step 1.1: `llm_node.h`**

```cpp
#ifndef AGENTFLOW_NODES_LLM_NODE_H_
#define AGENTFLOW_NODES_LLM_NODE_H_

#include <memory>
#include <string>
#include <string_view>

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/node.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/tools/tool_registry.h"
#include "c/engine.h"  // LiteRtLmSamplerParams

namespace agentflow {

struct LlmNodeConfig {
  std::string id;
  std::shared_ptr<LiteRtLmEngine> engine;
  asio::io_context* io_ctx = nullptr;

  std::string system_prompt;

  LiteRtLmSamplerParams sampler{};
  int max_output_tokens = 512;
  bool stream_tokens = true;

  // Same protobuf-field-name conventions AgentNode uses, so LlmNode and
  // AgentNode can read/write the same State shape interchangeably.
  std::string input_field;       // read user query from this string field
  std::string output_field;      // write assistant reply into this string field
  std::string messages_field;    // optional repeated msg field to append turn into

  // Optional: publish tool schemas in the conversation prompt so the model
  // can emit function-calling JSON. LlmNode does NOT execute the call — the
  // raw model reply (which may carry a `tool_calls` array) is written to
  // output_field for the next node (AgentNode / a downstream tool node) to
  // dispatch. Leave tool_registry null for plain completion.
  std::shared_ptr<ToolRegistry> tool_registry;
  std::vector<std::string> tool_names;  // empty + non-null registry = expose all
};

class LlmNode : public Node {
 public:
  explicit LlmNode(LlmNodeConfig cfg);
  ~LlmNode() override = default;

  std::string_view Id() const override { return cfg_.id; }
  std::string_view Kind() const override { return "llm"; }

  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;

 private:
  std::string BuildConversationJson(const State& state) const;
  void WriteOutput(State& state, const std::string& text) const;

  LlmNodeConfig cfg_;
};

}  // namespace agentflow
#endif
```

- [ ] **Step 1.2: `llm_node.cc` — single-shot `Run`**

```cpp
#include "agentflow/nodes/llm_node.h"

#include <utility>

#include <nlohmann/json.hpp>

#include "agentflow/core/errors.h"
#include "agentflow/inference/litert_lm_session.h"
#include "c/engine.h"

namespace agentflow {

namespace {
using json = nlohmann::json;

// Borrow AgentNode's reflection helpers verbatim (Read/Write the same string
// field via google::protobuf::Reflection). If/when these are reused by a
// third node, factor into agentflow/nodes/state_field_util.h.
std::string ReadField(const State& s, const std::string& f) {
  const auto* msg = s.UnsafeMessage();
  if (!msg) return {};
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(f);
  if (!desc) return {};
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING)
    return refl->GetString(*msg, desc);
  return {};
}

void WriteField(State& s, const std::string& f, const std::string& v) {
  auto* msg = const_cast<google::protobuf::Message*>(s.UnsafeMessage());
  if (!msg) return;
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(f);
  if (!desc) return;
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING)
    refl->SetString(msg, desc, v);
}
}  // namespace

LlmNode::LlmNode(LlmNodeConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.id.empty()) throw AgentflowError("LlmNode: id required");
  if (!cfg_.engine) throw AgentflowError("LlmNode: engine required");
  if (!cfg_.io_ctx) throw AgentflowError("LlmNode: io_ctx required");
}

std::string LlmNode::BuildConversationJson(const State& state) const {
  json msgs = json::array();
  if (!cfg_.system_prompt.empty()) {
    msgs.push_back({{"role", "system"}, {"content", cfg_.system_prompt}});
  }
  // (messages_field history append left as a follow-up; the AgentNode flow
  // also currently no-ops on history in the same place — keep parity.)
  msgs.push_back(
      {{"role", "user"}, {"content", ReadField(state, cfg_.input_field)}});

  json full;
  full["messages"] = msgs;
  full["max_tokens"] = cfg_.max_output_tokens;
  full["stream"] = cfg_.stream_tokens;

  // Publish tool schemas so the model can emit function-calling JSON. We do
  // not dispatch the call here — that's the caller's job.
  if (cfg_.tool_registry) {
    auto tools_json =
        cfg_.tool_registry->ExportToolsJson(cfg_.tool_names);
    full["tools"] = json::parse(tools_json);
  }
  return full.dump();
}

void LlmNode::WriteOutput(State& state, const std::string& text) const {
  WriteField(state, cfg_.output_field, text);
}

asio::awaitable<State> LlmNode::Run(State state, const CancelToken& cancel,
                                     EventEmitter& emit) {
  if (cancel.IsCancelled()) co_return std::move(state);

  auto* raw_session = litert_lm_engine_create_session(
      cfg_.engine->Get(), /*session_config=*/nullptr);
  if (!raw_session) {
    throw AgentflowError("LlmNode: failed to create LiteRT-LM session");
  }
  LiteRtLmSession session(raw_session, *cfg_.io_ctx);

  emit.EmitNodeStart(Id());
  session.Start(BuildConversationJson(state));

  std::string accum;
  while (true) {
    if (cancel.IsCancelled()) break;
    std::string tok = co_await session.NextTokenAsync();
    if (tok.empty()) break;
    accum += tok;
    if (cfg_.stream_tokens) emit.EmitToken(Id(), tok);
  }
  emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);

  WriteOutput(state, accum);
  co_return std::move(state);
}

}  // namespace agentflow
```

- [ ] **Step 1.3: BUILD.bazel**

```python
cc_library(
    name = "nodes",
    srcs = [
        "agent_node.cc",
        "llm_node.cc",
    ],
    hdrs = [
        "agent_node.h",
        "llm_node.h",
    ],
    deps = [
        "//agentflow/core",
        "//agentflow/inference",
        "//agentflow/tools",
        "@asio",
        "@nlohmann_json//:json",
    ],
)
```

(team_node / router_node / aggregator_node get added in their own tasks.)

- [ ] **Step 1.4: Commit**

```bash
git add agentflow/nodes/llm_node.{h,cc} agentflow/nodes/BUILD.bazel
git commit -m "nodes: add LlmNode — single LLM call, no tools, no ReAct loop"
```

**Exit criterion:** `bazel build //agentflow/nodes:nodes` passes. The unit test (stub-driven smoke + a DISABLED real-LLM case mirroring `agent_node_test`) lands in T6.

---

## Task 2: `TeamNode` skeleton + `StateRouter` policy

**Files:** Create `agentflow/nodes/team_node.{h,cc}`; modify `agentflow/nodes/BUILD.bazel`.

**Context:** Per spec §5.3, `TeamNode` "对外是单 graph node, 内部跑 reason→act→observe 循环, 不递归调用 Runner". The simplest policy is `StateRouter` — a user-supplied function reads the current `State` and returns the `NodeId` of the next member to run. Loop until the function returns empty (= done) or `max_turns` is reached.

- [ ] **Step 2.1: `team_node.h`**

```cpp
#ifndef AGENTFLOW_NODES_TEAM_NODE_H_
#define AGENTFLOW_NODES_TEAM_NODE_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/node.h"
#include "agentflow/core/state.h"

namespace agentflow {

struct TeamNodeConfig {
  enum class Policy { StateRouter, ParallelGather, LlmSelect };
  Policy policy;

  std::string id;  // graph-level NodeId
  std::vector<std::unique_ptr<Node>> members;  // owned

  // Cap on the orchestration loop (LlmSelect / StateRouter). Default mirrors
  // AgentNode's max_iter so users don't have to think about two loops.
  int max_turns = 8;

  // ── StateRouter ───────────────────────────────────────────────────────────
  // Receives the current State; returns the next member's Id, or empty to
  // stop. Called under team_node ownership of `state` — read-only access.
  std::function<std::string(const State&)> router;

  // ── ParallelGather ────────────────────────────────────────────────────────
  // Receives one State per member (same order as `members`). Returns the
  // merged State that the runner will fan out. If null, last-writer-wins.
  std::function<State(std::vector<State>)> aggregator;

  // ── LlmSelect (T3) ────────────────────────────────────────────────────────
  // Filled in by Task 3. Left declared here to keep ABI stable across tasks.
  // (LlmNode::Config moderator_llm; -- declared once we factor LlmNode out,
  //  see Task 3 notes.)
};

class TeamNode : public Node {
 public:
  explicit TeamNode(TeamNodeConfig cfg);
  ~TeamNode() override = default;

  std::string_view Id() const override { return cfg_.id; }
  std::string_view Kind() const override { return "team"; }

  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;

 private:
  asio::awaitable<State> RunStateRouter(State state, const CancelToken& cancel,
                                         EventEmitter& emit);
  asio::awaitable<State> RunParallelGather(State state,
                                            const CancelToken& cancel,
                                            EventEmitter& emit);
  asio::awaitable<State> RunLlmSelect(State state, const CancelToken& cancel,
                                       EventEmitter& emit);  // Task 3

  Node* FindMember(std::string_view id) const;

  TeamNodeConfig cfg_;
};

}  // namespace agentflow
#endif
```

- [ ] **Step 2.2: `team_node.cc` — `Run` dispatch + `RunStateRouter`**

```cpp
#include "agentflow/nodes/team_node.h"

#include <utility>

#include "agentflow/core/errors.h"

namespace agentflow {

TeamNode::TeamNode(TeamNodeConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.id.empty()) {
    throw AgentflowError("TeamNode: id must be set");
  }
  if (cfg_.members.empty()) {
    throw AgentflowError("TeamNode: members must be non-empty");
  }
}

Node* TeamNode::FindMember(std::string_view id) const {
  for (const auto& m : cfg_.members) {
    if (m->Id() == id) return m.get();
  }
  return nullptr;
}

asio::awaitable<State> TeamNode::Run(State state, const CancelToken& cancel,
                                      EventEmitter& emit) {
  switch (cfg_.policy) {
    case TeamNodeConfig::Policy::StateRouter:
      co_return co_await RunStateRouter(std::move(state), cancel, emit);
    case TeamNodeConfig::Policy::ParallelGather:
      co_return co_await RunParallelGather(std::move(state), cancel, emit);
    case TeamNodeConfig::Policy::LlmSelect:
      co_return co_await RunLlmSelect(std::move(state), cancel, emit);
  }
  throw AgentflowError("TeamNode: unknown policy");
}

asio::awaitable<State> TeamNode::RunStateRouter(State state,
                                                 const CancelToken& cancel,
                                                 EventEmitter& emit) {
  if (!cfg_.router) {
    throw AgentflowError("TeamNode StateRouter: router fn required");
  }
  for (int turn = 0; turn < cfg_.max_turns; ++turn) {
    if (cancel.IsCancelled()) co_return std::move(state);
    std::string next = cfg_.router(state);
    if (next.empty()) co_return std::move(state);  // router decided "done"
    Node* member = FindMember(next);
    if (!member) {
      throw AgentflowError("TeamNode: router returned unknown member id: " +
                           next);
    }
    emit.EmitNodeStart(Id());
    state = co_await member->Run(std::move(state), cancel, emit);
    emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);
  }
  co_return std::move(state);  // max_turns reached
}

asio::awaitable<State> TeamNode::RunParallelGather(State /*state*/,
                                                    const CancelToken& /*cancel*/,
                                                    EventEmitter& /*emit*/) {
  throw AgentflowError("TeamNode ParallelGather: not implemented (Task 2)");
}

asio::awaitable<State> TeamNode::RunLlmSelect(State /*state*/,
                                               const CancelToken& /*cancel*/,
                                               EventEmitter& /*emit*/) {
  throw AgentflowError("TeamNode LlmSelect: not implemented (Task 3)");
}

}  // namespace agentflow
```

- [ ] **Step 2.3: BUILD.bazel**

Append to `agentflow/nodes/BUILD.bazel`:

```python
cc_library(
    name = "nodes",
    srcs = [
        "agent_node.cc",
        "team_node.cc",
    ],
    hdrs = [
        "agent_node.h",
        "team_node.h",
    ],
    deps = [
        "//agentflow/core",
        "//agentflow/inference",
        "//agentflow/tools",
        "@asio",
        "@nlohmann_json//:json",
    ],
)
```

- [ ] **Step 2.4: Commit**

```bash
git add agentflow/nodes/team_node.{h,cc} agentflow/nodes/BUILD.bazel
git commit -m "nodes: add TeamNode skeleton + StateRouter policy"
```

**Exit criterion:** `bazel build //agentflow/nodes:nodes` passes. Unit tests for StateRouter land in Task 5.

---

## Task 3: `TeamNode` `ParallelGather` policy

**Files:** Modify `team_node.cc`.

**Context:** Fan members out concurrently on the same `io_context`, await all, then merge with `cfg_.aggregator`. Each member receives an independent clone of the entry State (so they don't race on a shared message).

- [ ] **Step 3.1: Implement `RunParallelGather`**

```cpp
asio::awaitable<State> TeamNode::RunParallelGather(State state,
                                                    const CancelToken& cancel,
                                                    EventEmitter& emit) {
  if (!cfg_.aggregator) {
    // Default: last-writer-wins (matches Runner's default fan-in policy from P1).
    cfg_.aggregator = [](std::vector<State> ins) {
      if (ins.empty()) return State{};
      return std::move(ins.back());
    };
  }
  emit.EmitNodeStart(Id());

  // Clone the entry state once per member so concurrent Run()s don't race on
  // a shared protobuf.
  using PerMemberChannel = asio::experimental::channel<
      void(asio::error_code, State)>;
  auto exec = co_await asio::this_coro::executor;
  std::vector<std::shared_ptr<PerMemberChannel>> done;
  done.reserve(cfg_.members.size());

  for (auto& member_up : cfg_.members) {
    auto ch = std::make_shared<PerMemberChannel>(exec, 1);
    done.push_back(ch);
    asio::co_spawn(exec,
        [&, member = member_up.get(), input = state.Clone(),
         ch]() mutable -> asio::awaitable<void> {
          State out;
          try {
            out = co_await member->Run(std::move(input), cancel, emit);
          } catch (...) {
            // Failure: propagate via channel close; the gather completes with
            // the inputs that succeeded.
            ch->close();
            co_return;
          }
          asio::error_code ec;
          ch->try_send(ec, std::move(out));
        },
        asio::detached);
  }

  std::vector<State> outs;
  outs.reserve(done.size());
  for (auto& ch : done) {
    auto [ec, s] = co_await ch->async_receive(
        asio::as_tuple(asio::use_awaitable));
    if (!ec) outs.push_back(std::move(s));
  }

  emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);
  co_return cfg_.aggregator(std::move(outs));
}
```

- [ ] **Step 3.2: Commit**

```bash
git commit -am "nodes: TeamNode ParallelGather — concurrent member fan-out + user merge"
```

**Exit criterion:** `bazel build //agentflow/nodes:nodes` passes. Unit tests land in Task 5.

---

## Task 4: `TeamNode` `LlmSelect` policy

**Files:** Modify `team_node.{h,cc}`.

**Context:** A moderator LLM picks the next member to run. Each turn:

1. Build a prompt that includes the State (or a digest of it) and the available member IDs + their `Kind()`.
2. Run the moderator (`LlmNode` from T1 — a single LLM call, no tool loop).
3. Parse the response for a chosen NodeId; if "DONE" or unparseable → stop.
4. Otherwise run the chosen member, append its output to State, loop until `max_turns`.

We use `LlmNode` (not `AgentNode`) as the moderator because the moderator's job is "produce one token stream and write it to `output_field`" — `LlmNode` is exactly that surface with no extra ReAct machinery in the path. The moderator config sets `tool_registry = nullptr` (plain completion); the chosen-member-id decision is plain text, not a tool call.

- [ ] **Step 4.1: Extend `TeamNodeConfig`**

```cpp
// Add to TeamNodeConfig:
//
// ── LlmSelect ────────────────────────────────────────────────────────────
// Moderator LLM config. The moderator's input_field receives the per-turn
// prompt naming the available members; its output_field is where we read the
// chosen member's id back from.
LlmNodeConfig moderator;
```

- [ ] **Step 4.2: `RunLlmSelect`**

```cpp
asio::awaitable<State> TeamNode::RunLlmSelect(State state,
                                               const CancelToken& cancel,
                                               EventEmitter& emit) {
  // Build a moderator LlmNode on the fly. It runs against a copy of the
  // state with a custom "turn prompt" written into its input_field; we read
  // the chosen member id back out of its output_field.
  LlmNode moderator(cfg_.moderator);

  for (int turn = 0; turn < cfg_.max_turns; ++turn) {
    if (cancel.IsCancelled()) co_return std::move(state);

    // Build the per-turn prompt naming the available members.
    // (Members register their own Schema via Kind(); we serialize a tiny
    // listing so the LLM can pick.)
    State turn_state = state.Clone();
    WriteTurnPrompt(turn_state, BuildMembersListing());

    turn_state = co_await moderator.Run(std::move(turn_state), cancel, emit);
    if (cancel.IsCancelled()) co_return std::move(state);

    std::string decision = ReadDecision(turn_state);
    if (decision.empty() || decision == "DONE") co_return std::move(state);

    Node* member = FindMember(decision);
    if (!member) {
      // LLM hallucinated a member id; surface to State as a moderator-error
      // entry the moderator can read next turn, then continue.
      AppendModeratorNote(state,
                          "moderator picked unknown member: " + decision);
      continue;
    }
    emit.EmitNodeStart(Id());
    state = co_await member->Run(std::move(state), cancel, emit);
    emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);
  }
  co_return std::move(state);
}
```

Helper bodies (`BuildMembersListing`, `WriteTurnPrompt`, `ReadDecision`, `AppendModeratorNote`) live in the .cc as static-namespace helpers. Each does a string read/write of the State's `messages_field` / `input_field` / `output_field` using the same reflection helpers AgentNode already uses (factor out into `state_field_util.h` if reused; otherwise inline).

- [ ] **Step 4.3: Commit**

```bash
git commit -am "nodes: TeamNode LlmSelect — LlmNode moderator drives member selection"
```

**Exit criterion:** `bazel build //agentflow/nodes:nodes` + the existing `agent-demo` binary still link.

---

## Task 5: `RouterNode` + `AggregatorNode`

**Files:** Create `router_node.{h,cc}`, `aggregator_node.{h,cc}`; modify `agentflow/nodes/BUILD.bazel`.

**Context:** Both are thin wrappers around user functions, no new graph-level mechanics. Per spec §5.4 they're "thin wrapper, 逻辑都是用户函数".

- [ ] **Step 5.1: `router_node.h`**

```cpp
namespace agentflow {

// Reads State, returns the NodeId of the downstream this graph should route to.
// The decision is written into a configurable string field on the output
// State; downstream nodes consult that field via their own logic (RouterNode
// itself doesn't perturb the runner's edge mechanics — the existing
// activation_group + per-edge condition handles the structural side).
struct RouterNodeConfig {
  std::string id;
  std::function<std::string(const State&)> chooser;
  std::string output_field;  // string field to write the chosen NodeId into
};

class RouterNode : public Node {
 public:
  explicit RouterNode(RouterNodeConfig cfg);
  std::string_view Id() const override { return cfg_.id; }
  std::string_view Kind() const override { return "router"; }
  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;
 private:
  RouterNodeConfig cfg_;
};

}  // namespace agentflow
```

- [ ] **Step 5.2: `router_node.cc`** — `Run` calls `chooser(state)`, writes the result into `output_field` via the protobuf-reflection helper, returns the state. Cancellation is checked at entry and is a no-op pass-through.

- [ ] **Step 5.3: `aggregator_node.{h,cc}`**

```cpp
struct AggregatorNodeConfig {
  std::string id;
  // Receives the current state (which the runner has already merged via its
  // fan-in policy) plus a configurable label string from State; returns the
  // post-aggregation State. Most users will just pass an identity fn and rely
  // on the runner's default last-writer-wins; AggregatorNode is the seam for
  // anything more sophisticated (e.g., majority vote on a string field).
  std::function<State(State)> merger;
  std::string id_field_str{};  // optional: write merger result tag here
};

class AggregatorNode : public Node {
 public:
  explicit AggregatorNode(AggregatorNodeConfig cfg);
  std::string_view Id() const override { return cfg_.id; }
  std::string_view Kind() const override { return "aggregator"; }
  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;
 private:
  AggregatorNodeConfig cfg_;
};
```

- [ ] **Step 5.4: BUILD + commit**

```bash
git add agentflow/nodes/{router_node,aggregator_node}.{h,cc} agentflow/nodes/BUILD.bazel
git commit -m "nodes: add RouterNode + AggregatorNode (thin user-fn wrappers)"
```

---

## Task 6: Unit tests

**Files:** `tests/unit/nodes/{llm,team,router,aggregator}_node_test.cc`; modify `tests/unit/nodes/BUILD.bazel`.

The unit tests must work without a real model. We reuse the `StubNode` helper from `tests/unit/core/stub_node.h` (a `Node` impl that runs a user-supplied lambda on the state with a configurable delay). `LlmNode` (T1) needs a real LiteRT-LM engine; its smoke test is tagged `manual` like `agent_node_test`, with an `EngineCfg_RequiresFields` ctor-validation test that runs unconditionally.

- [ ] **Step 6.1: `llm_node_test.cc`**

| Test | Asserts |
|---|---|
| `CtorRejectsMissingFields` | omit `id`/`engine`/`io_ctx` → AgentflowError. |
| `DISABLED_SingleCallWritesOutput` | (manual, needs `MODEL_PATH`) Run on a one-line state → output_field populated with non-empty text. |

- [ ] **Step 6.2: `team_node_test.cc`**

| Test | Asserts |
|---|---|
| `StateRouter_RouterDecidesOrderAndStops` | router returns "a", "b", "" → members a, b run in order; loop stops on empty. |
| `StateRouter_StopsAtMaxTurns` | router always returns "a" → exactly max_turns invocations of a. |
| `StateRouter_RouterReturnsUnknownThrows` | router returns "ghost" → AgentflowError. |
| `StateRouter_CancelExitsBetweenTurns` | cancel mid-loop → loop exits without running another member. |
| `ParallelGather_RunsAllMembersConcurrently` | 3 members, each delaying 50ms → total wall time < 100ms. |
| `ParallelGather_AggregatorMerges` | custom aggregator concatenates outputs → final State reflects merge. |
| `ParallelGather_DefaultAggregatorLastWrites` | no aggregator + 3 members → final State == last member's output. |
| `ParallelGather_MemberThrowSkipped` | one member throws → gather completes with the rest; aggregator sees only successful outputs. |

(LlmSelect tests are tagged `manual` and require a real model — same pattern as `agent_node_test`.)

- [ ] **Step 6.3: `router_node_test.cc`**

| Test | Asserts |
|---|---|
| `WritesChooserResultIntoOutputField` | chooser returns "b"; output_field on State contains "b". |
| `CancelShortCircuits` | pre-cancelled token → no chooser call, State unchanged. |
| `ChooserThrowPropagates` | chooser throws → AgentflowError propagates out. |

- [ ] **Step 6.4: `aggregator_node_test.cc`**

| Test | Asserts |
|---|---|
| `IdentityMergerPassesStateThrough` | identity merger → output equals input. |
| `CustomMergerTransforms` | merger increments a counter field → output reflects change. |

- [ ] **Step 6.5: BUILD + run + commit**

```bash
bazel test //tests/unit/nodes/... --test_output=errors
git add tests/unit/nodes/
git commit -m "test: LlmNode (ctor), TeamNode (StateRouter+ParallelGather), Router, Aggregator"
```

LlmSelect tests are deferred (need MODEL_PATH); the policy is exercised end-to-end in Task 7's demo if a model is available.

---

## Task 7: `team-demo` + wrap-up verification

**Files:** Create `examples/team-demo/{main.cc,BUILD.bazel}`.

**Context:** A small standalone binary that shows StateRouter + ParallelGather flows using stub members (so it runs in CI without a model). Mirrors `examples/core-stub-graph` in spirit.

- [ ] **Step 7.1: `main.cc`** — Builds a graph: `entry → team_state_router → team_parallel_gather → sink`. Each TeamNode has 2–3 `StubNode` members that print their id when run. Prints the trace events.

- [ ] **Step 7.2: BUILD + build**

```python
cc_binary(
    name = "team_demo",
    srcs = ["main.cc"],
    deps = ["//agentflow/core", "//agentflow/nodes"],
)
```

- [ ] **Step 7.3: Full verification**

```bash
bazel test //tests/unit/core/... //tests/unit/tools/... //tests/unit/nodes/... \
            //tests/integration/tools/... --test_output=errors
bazel run //examples/team-demo
bazel build //examples/agent-demo:agent_demo  # still links
```

Expected: P1 (39), P2 tools (5), P3 tools-mcp (14), P3 integration (1), P4 nodes (~14 new — LlmNode ctor + Team + Router + Aggregator) all pass. Demo prints expected sequence + ParallelGather fans out 3-wide.

- [ ] **Step 7.4: Tag**

```bash
git tag -a p4-multi-agent -m "P4: LlmNode + TeamNode (3 policies) + RouterNode + AggregatorNode"
```

- [ ] **Step 7.5: Commit**

```bash
git add examples/team-demo/
git commit -m "examples: add team-demo proving TeamNode StateRouter + ParallelGather work"
```

---

## Self-Review

**Spec coverage:**
- §5.1 LlmNode → T1
- §5.3 TeamNode (LlmSelect / StateRouter / ParallelGather) → T2, T3, T4
- §5.4 RouterNode + AggregatorNode → T5
- §15 Demo B (planner → researcher → writer with TeamNode LlmSelect) → unblocked by T4; full demo gated on a real model and lands as a follow-up

**Explicitly out of scope:**
- AgentNode-as-LlmNode refactor — `AgentNode` stays as-is (ReAct loop intact); `LlmNode` is purely additive. Refactoring AgentNode to be built on top of LlmNode is a P5+ cleanup, not blocking.
- Graph-level `chooser`/`is_routed_to` edge conditions — RouterNode publishes a decision string and downstream nodes consult it via their own logic. Adding a structural edge predicate is a P5+ runner concern.
- Recursive `TeamNode` (a TeamNode whose member is another TeamNode) is supported by virtue of the `Node` interface; the LlmSelect moderator config can be reused. Tested only in passing.

**Type consistency:**
- All three nodes implement the existing `Node` interface (Id/Kind/Run); the runner is unchanged.
- `TeamNodeConfig::members` holds `unique_ptr<Node>` so anything that fits the interface composes (including AgentNode, NativeFnTool-wrapping nodes, future LlmNode, even another TeamNode).
- Cancellation is checked at every loop iteration boundary (StateRouter / LlmSelect) and once per parallel branch (ParallelGather), matching P1/P2's contract.

**Known risks:**
1. **ParallelGather + shared `EventEmitter`** — the existing `EventEmitter` may not be thread-safe across the parallel `co_spawn`s. Mitigation: each branch runs on the same `io_context` thread (single-threaded driver assumed); revisit if/when we go multi-threaded.
2. **Moderator JSON parsing brittleness (LlmSelect)** — the LLM might return free text instead of a bare member id. Mitigation: tolerate both `"a"` and `{"choice":"a"}` shapes in `ReadDecision`; fall through to a moderator note + retry.
3. **State.Clone() cost in ParallelGather** — each member gets a clone of the entry state. For very large State (e.g., long message histories), this is O(n × members). Acceptable for v1; revisit when persistence/checkpointing lands in P5.

---

## Execution Handoff

**Plan saved to `docs/superpowers/plans/2026-06-01-cpp-agent-framework-p4-multi-agent.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

**Which approach?**
