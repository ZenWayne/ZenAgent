# Parallel Tool Dispatch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make AgentNode's dispatch loop execute all tool calls of a turn concurrently and deliver a deep-search example (Tavily) validated by real-cloud e2e plus a local gemma-4 wall-time comparison.

**Architecture:** Parallelism lives entirely in `AgentNode::Run`'s dispatch loop: normalize the turn's tool calls, `co_spawn` one dispatch coroutine per call with one `asio::experimental::channel` per call (the same ResultChannel pattern `TeamNode::RunParallelGather` uses), await the channels in original call order, and emit one tool-role message with 1:1 aligned results. No `parallel` flag anywhere — proto/loader changes are comments and error wording only.

**Tech Stack:** C++20 coroutines (asio), protobuf, nlohmann/json, Bazel, gtest. Tavily REST API over the existing `HttpsClient` (POST only).

**Spec:** `docs/superpowers/specs/2026-08-30-parallel-tool-dispatch-design.md`

## Global Constraints

- C++20, Bazel build; all new code under `agentflow/`, `examples/deep-search/`, `tests/` following existing file conventions.
- `schema_version` stays 1; `DelegateSpec` field 3 stays `reserved "parallel"`.
- No `parallel` key/flag/parameter is added anywhere; the workflow loader keeps rejecting a static `"parallel"` JSON key (reworded message).
- Credentials never in workflow JSON: `TAVILY_API_KEY` / `AGENTFLOW_LLM_*` come from the environment in host code only.
- Behavioural invariant (existing tests must keep passing unchanged): a non-object tool-call entry is skipped entirely (no result slot); every object entry produces exactly one result slot (empty-name dispatch when fields are malformed); `tool_call_id` echoed back per slot.
- `delegate_tool.cc`, `sub_agent_runtime.*`, `workflow_runner.cc`, `team_node.*` are NOT modified.
- Build: `bazel build //agentflow/...` and `bazel test //tests/unit/...` must stay green. (Proxy note: `bazel` may need `--host_jvm_args=-Dhttps.proxyHost=127.0.0.1 --host_jvm_args=-Dhttps.proxyPort=10808` on this machine.)

---

### Task 1: Failing tests for concurrent dispatch

**Files:**
- Modify: `tests/unit/nodes/agent_node_test.cc` (append tests before the closing `}  // namespace agentflow`)

**Interfaces:**
- Consumes: `tests/support/fake_chat_backend.h` (`agentflow::testing::FakeChatBackend`, `FakeConversation::sent()`), existing helpers in the test file (`BaseConfig`, `RunNode`, `EventCapture`), `agentflow::NativeFnTool`, `agentflow::ToolRegistry`, `agentflow::AgentNode`, `test_messages.pb.h` (`agentflow::test::TestState`).
- Produces: four new tests that must FAIL against the current sequential dispatch loop: `MultipleToolCallsInOneTurnRunConcurrently`, `ParallelResultsKeepOriginalCallOrderAndIds`, `EscapingToolExceptionYieldsErrorPlaceholderInPlace`, `CancellationPropagatesToSpawnedToolCalls`.

- [ ] **Step 1: Add includes needed by the new tests**

At the top of `tests/unit/nodes/agent_node_test.cc`, after the existing `#include "asio/use_future.hpp"` line, add:

```cpp
#include <asio/experimental/channel.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
```

and after `#include "agentflow/tools/tool_registry.h"` add:

```cpp
#include <algorithm>
#include <chrono>
```

- [ ] **Step 2: Write test 1 — concurrency**

Append before the final `}  // namespace agentflow`:

```cpp
TEST(AgentNodeTest, MultipleToolCallsInOneTurnRunConcurrently) {
  asio::io_context io;
  // Turn 1: two tool calls. Turn 2: final answer.
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[)"
          R"({"id":"a","function":{"name":"tool_a","arguments":"{}"}},)"
          R"({"id":"b","function":{"name":"tool_b","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  auto timeline = std::make_shared<std::vector<std::string>>();
  auto registry = std::make_shared<ToolRegistry>();
  auto add_tool = [&](std::string name, std::chrono::milliseconds delay) {
    registry->Register(std::make_shared<NativeFnTool>(
        ToolSchema{.name = name,
                   .description = "test tool",
                   .params_json_schema = R"({"type":"object","properties":{}})"},
        [name, delay, timeline](std::string_view, const CancelToken&)
            -> asio::awaitable<std::string> {
          timeline->push_back(name + "_start");
          if (delay.count() > 0) {
            auto exec = co_await asio::this_coro::executor;
            asio::steady_timer t(exec, delay);
            co_await t.async_wait(asio::use_awaitable);
          }
          timeline->push_back(name + "_end");
          co_return name + "_result";
        }));
  };
  add_tool("tool_a", std::chrono::milliseconds(50));
  add_tool("tool_b", std::chrono::milliseconds(0));

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = registry;
  EventCapture cap;
  State out = RunNode(std::move(cfg), "go", io, cap);
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "done");

  // Concurrency evidence: tool_b STARTS before tool_a FINISHES. A sequential
  // dispatch loop would produce a_start, a_end, b_start, b_end instead.
  ASSERT_EQ(timeline->size(), 4u);
  EXPECT_EQ((*timeline)[0], "tool_a_start");
  auto b_start = std::find(timeline->begin(), timeline->end(), "tool_b_start");
  auto a_end = std::find(timeline->begin(), timeline->end(), "tool_a_end");
  ASSERT_NE(b_start, timeline->end());
  ASSERT_NE(a_end, timeline->end());
  EXPECT_LT(std::distance(timeline->begin(), b_start),
            std::distance(timeline->begin(), a_end));
}
```

- [ ] **Step 3: Write test 2 — result order and ids**

```cpp
TEST(AgentNodeTest, ParallelResultsKeepOriginalCallOrderAndIds) {
  asio::io_context io;
  // Turn 1: three calls where call_1 is SLOW (completion order differs from
  // call order). Turn 2: final answer.
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[)"
          R"({"id":"call_1","function":{"name":"slow","arguments":"{}"}},)"
          R"({"id":"call_2","function":{"name":"fast","arguments":"{}"}},)"
          R"({"id":"call_3","function":{"name":"fast","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  auto registry = std::make_shared<ToolRegistry>();
  auto add_tool = [&](std::string name, std::string canned,
                      std::chrono::milliseconds delay) {
    registry->Register(std::make_shared<NativeFnTool>(
        ToolSchema{.name = name,
                   .description = "test tool",
                   .params_json_schema = R"({"type":"object","properties":{}})"},
        [canned = std::move(canned), delay](std::string_view, const CancelToken&)
            -> asio::awaitable<std::string> {
          if (delay.count() > 0) {
            auto exec = co_await asio::this_coro::executor;
            asio::steady_timer t(exec, delay);
            co_await t.async_wait(asio::use_awaitable);
          }
          co_return canned;
        }));
  };
  add_tool("slow", "R1", std::chrono::milliseconds(50));
  add_tool("fast", "R2", std::chrono::milliseconds(0));

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = registry;
  EventCapture cap;
  State out = RunNode(std::move(cfg), "go", io, cap);
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "done");

  // The tool-role message must preserve the ORIGINAL call order and echo each
  // originating id, regardless of completion order.
  auto conv = backend->last_conversation();
  ASSERT_NE(conv, nullptr);
  ASSERT_EQ(conv->sent().size(), 2u);
  json tool_msg = json::parse(conv->sent()[1]);
  EXPECT_EQ(tool_msg["role"], "tool");
  ASSERT_EQ(tool_msg["content"].size(), 3u);
  EXPECT_EQ(tool_msg["content"][0]["id"], "call_1");
  EXPECT_EQ(tool_msg["content"][0]["response"]["value"], "R1");
  EXPECT_EQ(tool_msg["content"][1]["id"], "call_2");
  EXPECT_EQ(tool_msg["content"][1]["response"]["value"], "R2");
  EXPECT_EQ(tool_msg["content"][2]["id"], "call_3");
  EXPECT_EQ(tool_msg["content"][2]["response"]["value"], "R2");
}
```

- [ ] **Step 4: Write test 3 — error placeholder in place**

```cpp
TEST(AgentNodeTest, EscapingToolExceptionYieldsErrorPlaceholderInPlace) {
  asio::io_context io;
  // Turn 1: good, BAD, good. The bad tool throws a NON-std::exception, which
  // escapes DispatchTool's `catch (const std::exception&)` and must surface as
  // the {"error":"tool_execution_failed"} placeholder in its own slot.
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[)"
          R"({"id":"call_1","function":{"name":"good","arguments":"{}"}},)"
          R"({"id":"call_2","function":{"name":"bad","arguments":"{}"}},)"
          R"({"id":"call_3","function":{"name":"good","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  auto registry = std::make_shared<ToolRegistry>();
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = "good",
                 .description = "test tool",
                 .params_json_schema = R"({"type":"object","properties":{}})"},
      [](std::string_view, const CancelToken&)
          -> asio::awaitable<std::string> { co_return std::string("OK"); }));
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = "bad",
                 .description = "test tool",
                 .params_json_schema = R"({"type":"object","properties":{}})"},
      [](std::string_view, const CancelToken&)
          -> asio::awaitable<std::string> { throw 42; }));

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = registry;
  EventCapture cap;
  State out = RunNode(std::move(cfg), "go", io, cap);
  EXPECT_EQ(out.As<test::TestState>().assistant_reply(), "done");

  auto conv = backend->last_conversation();
  ASSERT_NE(conv, nullptr);
  json tool_msg = json::parse(conv->sent()[1]);
  ASSERT_EQ(tool_msg["content"].size(), 3u);
  EXPECT_EQ(tool_msg["content"][0]["response"]["value"], "OK");
  EXPECT_EQ(tool_msg["content"][1]["response"]["value"],
            R"({"error":"tool_execution_failed"})");
  EXPECT_EQ(tool_msg["content"][1]["id"], "call_2");  // slot keeps its id
  EXPECT_EQ(tool_msg["content"][2]["response"]["value"], "OK");
}
```

- [ ] **Step 5: Write test 4 — cancellation propagates to spawned calls**

```cpp
TEST(AgentNodeTest, CancellationPropagatesToSpawnedToolCalls) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","tool_calls":[)"
          R"({"id":"a","function":{"name":"t","arguments":"{}"}},)"
          R"({"id":"b","function":{"name":"t","arguments":"{}"}}]})",
          R"({"role":"assistant","content":[{"type":"text","text":"done"}]})"});

  auto registry = std::make_shared<ToolRegistry>();
  registry->Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = "t",
                 .description = "test tool",
                 .params_json_schema = R"({"type":"object","properties":{}})"},
      [](std::string_view, const CancelToken& cancel)
          -> asio::awaitable<std::string> {
        if (cancel.IsCancelled()) co_return std::string("cancelled");
        auto exec = co_await asio::this_coro::executor;
        asio::steady_timer t(exec, std::chrono::milliseconds(20));
        co_await t.async_wait(asio::use_awaitable);
        if (cancel.IsCancelled()) co_return std::string("cancelled");
        co_return std::string("done");
      }));

  auto cfg = BaseConfig(backend, io);
  cfg.tool_registry = registry;
  auto node = std::make_unique<AgentNode>(std::move(cfg));
  test::TestState raw;
  raw.set_user_query("go");
  EventCapture cap;
  CancelSource cancel;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node->Run(State::From(std::move(raw)),
                                     cancel.Token(), cap.emitter);
      },
      asio::use_future);
  // Cancel 5ms in — mid tool execution, before the tools' 20ms timers fire.
  asio::steady_timer kill(io, std::chrono::milliseconds(5));
  kill.async_wait([&](asio::error_code) { cancel.Cancel(); });
  io.run();
  State out = fut.get();

  // Both spawned tool coroutines observed the cancel (never a hang/crash).
  auto conv = backend->last_conversation();
  ASSERT_NE(conv, nullptr);
  ASSERT_GE(conv->sent().size(), 2u);
  json tool_msg = json::parse(conv->sent()[1]);
  ASSERT_EQ(tool_msg["content"].size(), 2u);
  EXPECT_EQ(tool_msg["content"][0]["response"]["value"], "cancelled");
  EXPECT_EQ(tool_msg["content"][1]["response"]["value"], "cancelled");
  // The run then stopped on the cancel check; the exact final text is not
  // asserted — only that cancellation propagated to every spawned coroutine.
  (void)out;
}
```

- [ ] **Step 6: Run the new tests — expect exactly two FAILURES**

Run: `bazel test //tests/unit/nodes:agent_node_test --test_filter='AgentNodeTest.MultipleToolCallsInOneTurnRunConcurrently:AgentNodeTest.ParallelResultsKeepOriginalCallOrderAndIds:AgentNodeTest.EscapingToolExceptionYieldsErrorPlaceholderInPlace:AgentNodeTest.CancellationPropagatesToSpawnedToolCalls'`
Expected: `MultipleToolCallsInOneTurnRunConcurrently` FAILS (the sequential loop produces `a_start, a_end, b_start, b_end`, so the `b_start before a_end` assertion fails) and `EscapingToolExceptionYieldsErrorPlaceholderInPlace` FAILS (`throw 42` aborts the run — no `catch(...)` wrapper yet). `ParallelResultsKeepOriginalCallOrderAndIds` and `CancellationPropagatesToSpawnedToolCalls` PASS under both implementations (they guard regressions, not the new behaviour) — their passing here is expected. Inspect the failure output of the two failing tests to confirm they fail for the reasons above before proceeding.

- [ ] **Step 7: Commit the failing tests**

```bash
git add tests/unit/nodes/agent_node_test.cc
git commit -m "test(nodes): add failing tests for concurrent tool dispatch"
```

---

### Task 2: Concurrent dispatch implementation in AgentNode

**Files:**
- Modify: `agentflow/nodes/agent_node.cc` (dispatch loop, roughly lines 145–213)

**Interfaces:**
- Consumes: `asio::experimental::channel<void(asio::error_code, std::string)>`, `asio::co_spawn`, `asio::this_coro::executor`; `DispatchTool(name, args, cancel, emit)` (existing, unchanged).
- Produces: concurrent dispatch inside `AgentNode::Run`; behaviour contract from Task 1's tests plus all pre-existing tests in `agent_node_test.cc`.

- [ ] **Step 1: Add includes**

In `agentflow/nodes/agent_node.cc`, the current include block is:

```cpp
#include "absl/status/status.h"
#include <asio/as_tuple.hpp>
#include <asio/use_awaitable.hpp>
```

Replace it with:

```cpp
#include "absl/status/status.h"
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
```

- [ ] **Step 2: Replace the dispatch loop**

Replace the current block from `if (resp.contains("tool_calls") && resp["tool_calls"].is_array() &&` down to (and including) `json tool_message = {{"role", "tool"}, {"content", tool_content}};` and its `continue;` — i.e. the entire tool-dispatch section — with:

```cpp
    if (resp.contains("tool_calls") && resp["tool_calls"].is_array() &&
        !resp["tool_calls"].empty()) {
      // Normalize all tool calls first. Model output is untrusted — never
      // read a field without proving BOTH that its container is an object
      // AND that the field has the type we are about to read it as (same
      // hardening as before). A non-object entry is skipped entirely (no
      // result slot); an object entry always produces one slot, degrading
      // to an empty-name dispatch when its fields are malformed.
      struct Call {
        std::string name;
        std::string call_id;
        std::string args;
      };
      std::vector<Call> calls;
      for (const auto& tc : resp["tool_calls"]) {
        if (!tc.is_object()) continue;
        Call c;
        if (tc.contains("name") && tc["name"].is_string()) {
          c.name = tc["name"].get<std::string>();
        } else if (tc.contains("function") && tc["function"].is_object()) {
          const json& fn = tc["function"];
          if (fn.contains("name") && fn["name"].is_string()) {
            c.name = fn["name"].get<std::string>();
          }
        }
        // The originating call's id. LiteRT-LM does not need it, but
        // OpenAI-compatible backends must echo it back as tool_call_id.
        if (tc.contains("id") && tc["id"].is_string()) {
          c.call_id = tc["id"].get<std::string>();
        }
        if (tc.contains("arguments")) {
          c.args = tc["arguments"].is_string()
                       ? tc["arguments"].get<std::string>()
                       : tc["arguments"].dump();
        } else if (tc.contains("function") &&
                   tc["function"].contains("arguments")) {
          c.args = tc["function"]["arguments"].is_string()
                       ? tc["function"]["arguments"].get<std::string>()
                       : tc["function"]["arguments"].dump();
        }
        calls.push_back(std::move(c));
      }

      // Concurrent dispatch: one channel per call — the same ResultChannel
      // pattern TeamNode::RunParallelGather uses. A closed channel means the
      // coroutine caught an exception DispatchTool's std::exception handler
      // cannot handle (e.g. a non-std throw); that yields an error
      // placeholder so the result array stays 1:1 with the calls.
      using ResultChannel =
          asio::experimental::channel<void(asio::error_code, std::string)>;
      auto exec = co_await asio::this_coro::executor;
      std::vector<std::shared_ptr<ResultChannel>> channels;
      channels.reserve(calls.size());
      for (const auto& c : calls) {
        auto ch = std::make_shared<ResultChannel>(exec, 1);
        channels.push_back(ch);
        asio::co_spawn(
            exec,
            [this, &cancel, &emit, ch, c]() mutable -> asio::awaitable<void> {
              try {
                std::string result =
                    co_await DispatchTool(c.name, c.args, cancel, emit);
                asio::error_code ec;
                ch->try_send(ec, std::move(result));
              } catch (...) {
                ch->close();
              }
            },
            asio::detached);
      }

      // Collect in ORIGINAL call order — completion order must not leak into
      // the tool-role message (OpenAI semantics: slots echo tool_call_id).
      json tool_content = json::array();
      for (size_t i = 0; i < calls.size(); ++i) {
        auto [ec, result] = co_await channels[i]->async_receive(
            asio::as_tuple(asio::use_awaitable));
        std::string value =
            ec ? std::string(R"({"error":"tool_execution_failed"})")
               : std::move(result);
        // Per the Gemma jinja template each content item carries `name` +
        // `response` directly; `id` is additive and ignored on that path.
        json entry = {
            {"name", calls[i].name},
            {"response", {{"value", value}}},
        };
        if (!calls[i].call_id.empty()) entry["id"] = calls[i].call_id;
        tool_content.push_back(std::move(entry));
      }
      json tool_message = {{"role", "tool"}, {"content", tool_content}};
      message_json = tool_message.dump();
      continue;
    }
```

Keep the original "Model output is untrusted..." comment lines: place them directly above the normalization loop (`for (const auto& tc : resp["tool_calls"])`). Do NOT delete the `DispatchTool` definition below the loop.

- [ ] **Step 3: Build**

Run: `bazel build //agentflow/nodes`
Expected: compile success, no warnings.

- [ ] **Step 4: Run the full agent_node test suite**

Run: `bazel test //tests/unit/nodes:agent_node_test`
Expected: ALL tests pass — the 4 new tests from Task 1 AND every pre-existing test (`ToolCallIsDispatchedAndItsIdEchoedBack`, `WrongTypedToolCallFieldsAreSkippedWithoutThrowing`, `NonObjectToolCallEntryIsSkippedWithoutThrowing`, `MaxIterReachedWritesTheFallbackMessage`, etc.), proving the malformed-entry semantics are preserved exactly.

- [ ] **Step 5: Commit**

```bash
git add agentflow/nodes/agent_node.cc
git commit -m "feat(nodes): dispatch multiple tool calls of a turn concurrently

One co_spawn'd dispatch coroutine per call, ResultChannel per call (the
TeamNode::RunParallelGather pattern), collected back in original call
order with 1:1 alignment. Escaping (non-std) exceptions yield an
{\"error\":\"tool_execution_failed\"} placeholder so slots never shift."
```

---

### Task 3: Proto comment + loader message + loader test update

**Files:**
- Modify: `proto/workflow_spec.proto` (comment block above `reserved 3;`)
- Modify: `agentflow/workflow/workflow_loader.cc` (the `parallel` rejection in `ParseDelegateSpec`)
- Modify: `tests/unit/workflow/workflow_loader_test.cc` (two tests)

**Interfaces:**
- Consumes: nothing new.
- Produces: reworded rejection; tests `RejectDelegatesParallelBecauseItIsNotImplemented` and `RejectDelegatesParallelEvenWhenSetToFalse` now assert the new wording.

- [ ] **Step 1: Rewrite the proto comment**

In `proto/workflow_spec.proto`, replace the comment block above `reserved 3;` / `reserved "parallel";` (currently the "TODO: implement genuine parallel delegation..." block) with:

```proto
    // Field 3 was `bool parallel`. Parallelism is now the DEFAULT behaviour
    // of AgentNode's tool-dispatch loop: multiple tool calls in one turn are
    // always executed concurrently (see the parallel-tool-dispatch design
    // spec). There is nothing to enable and no switch to disable, so the key
    // stays reserved and workflow_loader rejects it, telling the author that
    // parallelism needs no declaration.
    reserved 3;
    reserved "parallel";
```

- [ ] **Step 2: Reword the loader rejection**

In `agentflow/workflow/workflow_loader.cc`, replace the `parallel` rejection block in `ParseDelegateSpec` (the `if (j.find("parallel") != j.end())` block) with:

```cpp
  if (j.find("parallel") != j.end()) {
    return absl::InvalidArgumentError(
        "delegates.parallel is not a supported key — parallel execution is "
        "the default behaviour of the tool-dispatch loop and needs no "
        "declaration. Remove it.");
  }
```

- [ ] **Step 3: Update the two loader tests**

In `tests/unit/workflow/workflow_loader_test.cc`:

Rename `TEST(WorkflowLoaderTest, RejectDelegatesParallelBecauseItIsNotImplemented)` to `TEST(WorkflowLoaderTest, RejectDelegatesParallelBecauseItNeedsNoDeclaration)` and replace its trailing assertion block:

```cpp
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "parallel"));
  // Rejected for being unimplemented, not merely for being an unknown key —
  // the message has to tell the author what is actually true.
  EXPECT_TRUE(
      absl::StrContains(wf_or.status().message(), "not implemented"));
```

with:

```cpp
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "parallel"));
  // The message must tell the author what is actually true: parallelism is
  // the default and needs no declaration at all.
  EXPECT_TRUE(
      absl::StrContains(wf_or.status().message(), "default behaviour"));
```

In `TEST(WorkflowLoaderTest, RejectDelegatesParallelEvenWhenSetToFalse)` the final assertion `EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "parallel"));` stays as-is (still valid).

- [ ] **Step 4: Run the loader tests**

Run: `bazel test //tests/unit/workflow:workflow_loader_test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add proto/workflow_spec.proto agentflow/workflow/workflow_loader.cc tests/unit/workflow/workflow_loader_test.cc
git commit -m "docs(workflow): parallelism is the default; reword static parallel rejection"
```

---

### Task 4: Tavily tools + unit tests

**Files:**
- Create: `examples/deep-search/tavily_tools.h`
- Create: `examples/deep-search/tavily_tools.cc`
- Create: `examples/deep-search/BUILD.bazel` (library target only for now)
- Create: `tests/unit/tools/tavily_tools_test.cc`
- Modify: `tests/unit/tools/BUILD.bazel` (add test target)

**Interfaces:**
- Consumes: `agentflow::net::HttpsClient` (`Post(req, cancel, out_head) -> StatusOr<std::string>`), `agentflow::NativeFnTool`, `agentflow::ToolSchema`, nlohmann/json, `tests/support/fake_http_client.h` (`agentflow::net::FakeHttpClient` with scripted `FakeHttpTurn{body, status, status_code}`).
- Produces:
  - `std::shared_ptr<agentflow::Tool> deep_search::MakeTavilySearchTool(agentflow::net::HttpsClient& http, std::string api_key);`
  - `std::shared_ptr<agentflow::Tool> deep_search::MakeTavilyExtractTool(agentflow::net::HttpsClient& http, std::string api_key);`
  - Tool names: `tavily_search`, `tavily_extract` (exact strings the workflow JSON and later tasks reference).

- [ ] **Step 1: Create `examples/deep-search/tavily_tools.h`**

```cpp
// examples/deep-search/tavily_tools.h
#ifndef EXAMPLES_DEEP_SEARCH_TAVILY_TOOLS_H_
#define EXAMPLES_DEEP_SEARCH_TAVILY_TOOLS_H_

#include <memory>
#include <string>

#include "agentflow/tools/tool.h"

namespace agentflow {
namespace net {
class HttpsClient;
}  // namespace net
}  // namespace agentflow

namespace deep_search {

// "tavily_search": POST https://api.tavily.com/search with {"query": ...,
// "max_results": 5, "search_depth": "basic"}. Returns a JSON string of
// [{title,url,content}] (trimmed) or {"error": "<message>"}.
std::shared_ptr<agentflow::Tool> MakeTavilySearchTool(
    agentflow::net::HttpsClient& http, std::string api_key);

// "tavily_extract": POST https://api.tavily.com/extract with {"urls": [...],
// "extract_depth": "basic", "format": "markdown"} (max 20 URLs; Tavily
// fetches them server-side). Returns a JSON string of
// [{url,raw_content}] plus failed_results, or {"error": "<message>"}.
std::shared_ptr<agentflow::Tool> MakeTavilyExtractTool(
    agentflow::net::HttpsClient& http, std::string api_key);

}  // namespace deep_search

#endif  // EXAMPLES_DEEP_SEARCH_TAVILY_TOOLS_H_
```

- [ ] **Step 2: Create `examples/deep-search/tavily_tools.cc`**

```cpp
// examples/deep-search/tavily_tools.cc
#include "examples/deep-search/tavily_tools.h"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include <asio/awaitable.hpp>

#include "agentflow/net/http_client.h"
#include "agentflow/net/https_client.h"
#include "agentflow/tools/native_fn_tool.h"

namespace deep_search {
namespace {

using json = nlohmann::json;

constexpr char kTavilyBase[] = "https://api.tavily.com";

agentflow::net::HttpRequest MakeRequest(std::string path, std::string body,
                                        std::string_view api_key) {
  agentflow::net::HttpRequest req;
  req.url = std::string(kTavilyBase) + path;
  req.body = std::move(body);
  req.headers.push_back({"Content-Type", "application/json"});
  req.headers.push_back(
      {"Authorization", std::string("Bearer ") + std::string(api_key)});
  return req;
}

}  // namespace

std::shared_ptr<agentflow::Tool> MakeTavilySearchTool(
    agentflow::net::HttpsClient& http, std::string api_key) {
  agentflow::ToolSchema schema{
      .name = "tavily_search",
      .description =
          "Search the web via Tavily. Returns title, url and content for "
          "each result.",
      .params_json_schema =
          R"({"type":"object","properties":{"query":{"type":"string"}},)"
          R"("required":["query"]})"};
  auto fn = [&http, api_key = std::move(api_key)](
                std::string_view args_json,
                const agentflow::CancelToken& cancel)
                -> asio::awaitable<std::string> {
    json args = json::parse(args_json, nullptr, /*allow_exceptions=*/false);
    std::string query =
        (args.is_object() && args.contains("query") &&
         args["query"].is_string())
            ? args["query"].get<std::string>()
            : std::string{};
    if (query.empty()) co_return std::string(R"({"error":"bad_args"})");
    json body = {{"query", query}, {"max_results", 5}, {"search_depth", "basic"}};
    auto resp = co_await http.Post(MakeRequest("/search", body.dump(), api_key),
                                   cancel);
    if (!resp.ok()) co_return std::string(R"({"error":"http_error"})");
    json parsed = json::parse(*resp, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() ||
        !parsed.contains("results") || !parsed["results"].is_array()) {
      co_return std::string(R"({"error":"bad_response"})");
    }
    json out = json::array();
    for (const auto& r : parsed["results"]) {
      if (!r.is_object()) continue;
      json item;
      if (r.contains("title") && r["title"].is_string())
        item["title"] = r["title"];
      if (r.contains("url") && r["url"].is_string()) item["url"] = r["url"];
      if (r.contains("content") && r["content"].is_string())
        item["content"] = r["content"];
      out.push_back(std::move(item));
    }
    co_return out.dump();
  };
  return std::make_shared<agentflow::NativeFnTool>(std::move(schema),
                                                    std::move(fn));
}

std::shared_ptr<agentflow::Tool> MakeTavilyExtractTool(
    agentflow::net::HttpsClient& http, std::string api_key) {
  agentflow::ToolSchema schema{
      .name = "tavily_extract",
      .description =
          "Read web pages via Tavily. Provide up to 20 URLs in one call; "
          "they are fetched server-side. Returns {url, raw_content} "
          "entries.",
      .params_json_schema =
          R"({"type":"object","properties":{"urls":{"type":"array",)"
          R"("items":{"type":"string"}}},"required":["urls"]})"};
  auto fn = [&http, api_key = std::move(api_key)](
                std::string_view args_json,
                const agentflow::CancelToken& cancel)
                -> asio::awaitable<std::string> {
    json args = json::parse(args_json, nullptr, /*allow_exceptions=*/false);
    if (!args.is_object() || !args.contains("urls") ||
        !args["urls"].is_array()) {
      co_return std::string(R"({"error":"bad_args"})");
    }
    json urls = json::array();
    for (const auto& u : args["urls"]) {
      if (u.is_string()) urls.push_back(u.get<std::string>());
      if (urls.size() >= 20) break;  // Tavily hard limit
    }
    if (urls.empty()) co_return std::string(R"({"error":"bad_args"})");
    json body = {{"urls", urls},
                 {"extract_depth", "basic"},
                 {"format", "markdown"}};
    auto resp = co_await http.Post(
        MakeRequest("/extract", body.dump(), api_key), cancel);
    if (!resp.ok()) co_return std::string(R"({"error":"http_error"})");
    json parsed = json::parse(*resp, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() ||
        !parsed.contains("results") || !parsed["results"].is_array()) {
      co_return std::string(R"({"error":"bad_response"})");
    }
    json out;
    out["results"] = json::array();
    for (const auto& r : parsed["results"]) {
      if (!r.is_object()) continue;
      json item;
      if (r.contains("url") && r["url"].is_string()) item["url"] = r["url"];
      if (r.contains("raw_content") && r["raw_content"].is_string()) {
        std::string raw = r["raw_content"].get<std::string>();
        constexpr size_t kMaxLen = 6000;  // trim huge pages for the LLM
        if (raw.size() > kMaxLen) raw.resize(kMaxLen);
        item["raw_content"] = std::move(raw);
      }
      out["results"].push_back(std::move(item));
    }
    if (parsed.contains("failed_results") &&
        parsed["failed_results"].is_array()) {
      out["failed_results"] = parsed["failed_results"];
    }
    co_return out.dump();
  };
  return std::make_shared<agentflow::NativeFnTool>(std::move(schema),
                                                    std::move(fn));
}

}  // namespace deep_search
```

- [ ] **Step 3: Create `examples/deep-search/BUILD.bazel` (library target)**

```python
# examples/deep-search/BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "tavily_tools",
    srcs = ["tavily_tools.cc"],
    hdrs = ["tavily_tools.h"],
    deps = [
        "//agentflow/net:https_client",
        "//agentflow/tools",
        "@abseil-cpp//absl/status:statusor",
        "@asio",
        "@nlohmann_json//:json",
    ],
)
```

(The `cc_binary` target is added in Task 5.)

- [ ] **Step 4: Write the unit tests — `tests/unit/tools/tavily_tools_test.cc`**

```cpp
// tests/unit/tools/tavily_tools_test.cc
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "examples/deep-search/tavily_tools.h"
#include "tests/support/fake_http_client.h"

namespace agentflow {
namespace {

using json = nlohmann::json;
using net::FakeHttpClient;
using net::FakeHttpTurn;
using net::HttpRequest;

// Runs one tool invocation on `io` and returns the result string.
std::string Invoke(std::shared_ptr<Tool> tool, std::string args,
                   asio::io_context& io) {
  CancelSource cancel;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await tool->Invoke(args, cancel.Token());
      },
      asio::use_future);
  io.run();
  return fut.get();
}

TEST(TavilyToolsTest, SearchSendsBearerAndParsesResults) {
  asio::io_context io;
  std::vector<FakeHttpTurn> turns(1);
  turns[0].status_code = 200;
  turns[0].body =
      R"({"query":"q","results":[{"title":"T","url":"https://e.com",)"
      R"("content":"C"}],"answer":null})";
  FakeHttpClient fake(std::move(turns));
  auto tool = deep_search::MakeTavilySearchTool(fake, "tvly-TEST");

  std::string out = Invoke(tool, R"({"query":"who is leo messi"})", io);
  json parsed = json::parse(out);
  ASSERT_TRUE(parsed.is_array());
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0]["title"], "T");
  EXPECT_EQ(parsed[0]["url"], "https://e.com");
  EXPECT_EQ(parsed[0]["content"], "C");

  // Request shape: URL, bearer header, and a body with the right fields.
  const std::vector<HttpRequest>& reqs = fake.requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].url, "https://api.tavily.com/search");
  bool has_bearer = false;
  for (const auto& h : reqs[0].headers) {
    if (h.first == "Authorization") {
      has_bearer = h.second == "Bearer tvly-TEST";
    }
  }
  EXPECT_TRUE(has_bearer);
  json body = json::parse(reqs[0].body);
  EXPECT_EQ(body["query"], "who is leo messi");
  EXPECT_EQ(body["max_results"], 5);
}

TEST(TavilyToolsTest, ExtractTrimsAndKeepsFailedResults) {
  asio::io_context io;
  std::vector<FakeHttpTurn> turns(1);
  turns[0].status_code = 200;
  turns[0].body =
      R"({"results":[{"url":"https://a.com","raw_content":"hello"}],)"
      R"("failed_results":[{"url":"https://b.com","error":"403"}]})";
  FakeHttpClient fake(std::move(turns));
  auto tool = deep_search::MakeTavilyExtractTool(fake, "tvly-TEST");

  std::string out = Invoke(tool, R"({"urls":["https://a.com","https://b.com"]})",
                           io);
  json parsed = json::parse(out);
  ASSERT_TRUE(parsed.contains("results"));
  EXPECT_EQ(parsed["results"][0]["url"], "https://a.com");
  EXPECT_EQ(parsed["results"][0]["raw_content"], "hello");
  ASSERT_TRUE(parsed.contains("failed_results"));
  EXPECT_EQ(parsed["failed_results"][0]["url"], "https://b.com");

  const std::vector<HttpRequest>& reqs = fake.requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].url, "https://api.tavily.com/extract");
  json body = json::parse(reqs[0].body);
  ASSERT_EQ(body["urls"].size(), 2u);
  EXPECT_EQ(body["urls"][0], "https://a.com");
}

TEST(TavilyToolsTest, HttpErrorSurfacesAsErrorJson) {
  asio::io_context io;
  std::vector<FakeHttpTurn> turns(1);
  turns[0].status_code = 401;
  turns[0].status = absl::UnauthenticatedError("unauthorized");
  FakeHttpClient fake(std::move(turns));
  auto tool = deep_search::MakeTavilySearchTool(fake, "tvly-BAD");

  std::string out = Invoke(tool, R"({"query":"x"})", io);
  json parsed = json::parse(out);
  EXPECT_TRUE(parsed.contains("error"));
}
```

- [ ] **Step 5: Add the test target to `tests/unit/tools/BUILD.bazel`**

```python
cc_test(
    name = "tavily_tools_test",
    size = "small",
    srcs = ["tavily_tools_test.cc"],
    deps = [
        "//agentflow/core",
        "//examples/deep-search:tavily_tools",
        "//tests/support:fake_http_client",
        "@abseil-cpp//absl/status",
        "@asio",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)
```

(Check `tests/unit/tools/BUILD.bazel`'s existing load line and match its style for the `cc_test` rule; existing targets there already use `cc_test` + googletest.)

- [ ] **Step 6: Run the tavily tests**

Run: `bazel test //tests/unit/tools:tavily_tools_test`
Expected: PASS (3 tests).

- [ ] **Step 7: Commit**

```bash
git add examples/deep-search/tavily_tools.h examples/deep-search/tavily_tools.cc examples/deep-search/BUILD.bazel tests/unit/tools/tavily_tools_test.cc tests/unit/tools/BUILD.bazel
git commit -m "feat(examples): tavily search/extract native tools over HttpsClient"
```

---

### Task 5: Deep-search workflow JSON + dual-backend host + build

**Files:**
- Create: `examples/deep-search/workflow.json`
- Create: `examples/deep-search/main.cc`
- Modify: `examples/deep-search/BUILD.bazel` (add `cc_binary` target)

**Interfaces:**
- Consumes: `WorkflowLoader::LoadFromFile`, `workflow::BuildAgentNode` / `AgentNodeBuildSpec` (fields: `workflow`, `agent_name`, `host_tools`, `io_ctx`, `backends`, `emit`, `input_field`, `output_field`, `max_iter`), `BuiltAgentNode{cfg, keepalive}`, `LiteRtLmEngine::Create` / `LiteRtLmChatBackend::Create` (local mode), `openai::OpenAiChatBackend::Create` (cloud mode), `tavily_tools` from Task 4, `test_messages.pb.h` (`agentflow::test::TestState`).
- Produces: `//examples/deep-search:deep_search` binary that runs the workflow against cloud (default) or local gemma-4 (when `MODEL_PATH` is set).

- [ ] **Step 1: Create `examples/deep-search/workflow.json`**

```json
{
  "name": "deep-search",
  "version": "1",
  "state": {
    "kind": "dynamic_json",
    "fields": {
      "user_query": {"type": "string"},
      "assistant_reply": {"type": "string"}
    }
  },
  "agents": {
    "deep_search": {
      "system_prompt": "You are a deep research agent. Follow this process:\n1. Break the user's question into 2-4 focused sub-questions.\n2. IMPORTANT: answer with ALL delegate calls for ALL sub-questions in ONE single turn (emit multiple tool_calls at once). Each call: agent=\"searcher\", goal=<one sub-question>.\n3. When all results come back, synthesize a final answer with citations (source URLs from the research results).",
      "model": {"backend": "cloud", "max_output_tokens": 2048},
      "tools": ["tavily_search", "tavily_extract"],
      "delegates": {
        "agents": ["searcher"],
        "max_depth": 2,
        "input_template": {"user_query": "{{tool.goal}}"},
        "goal_template": "{{tool.goal}}",
        "output_extract": "$.assistant_reply"
      }
    },
    "searcher": {
      "system_prompt": "You research ONE sub-question. Use tavily_search to find sources, then tavily_extract to read the most promising pages. Return a concise research report: key findings plus the source URLs.",
      "model": {"backend": "cloud", "max_output_tokens": 1024},
      "tools": ["tavily_search", "tavily_extract"]
    }
  },
  "main": "deep_search"
}
```

- [ ] **Step 2: Create `examples/deep-search/main.cc`**

```cpp
// examples/deep-search/main.cc
//
// Deep-search host. Backend chosen by environment:
//   - MODEL_PATH set       -> local LiteRT-LM engine (gemma-4-E2B-it.litertlm)
//   - MODEL_PATH unset     -> OpenAI-compatible cloud endpoint
// Both register under the logical name "cloud" that workflow.json uses, so
// the identical workflow runs unchanged in either mode.
//
//   export TAVILY_API_KEY=tvly-...
//   # cloud:
//   export AGENTFLOW_LLM_BASE_URL=https://api.deepseek.com/v1
//   export AGENTFLOW_LLM_MODEL=deepseek-chat
//   export AGENTFLOW_LLM_API_KEY=sk-...
//   bazel run //examples/deep-search:deep_search -- "your question"
//   # local (wall-time comparison):
//   MODEL_PATH=models/gemma-4-E2B-it.litertlm \
//     bazel run //examples/deep-search:deep_search -- "your question"
//
// Credentials live only in the environment (host code) — never in
// workflow.json.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/litert_lm_chat_backend.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/inference/openai/openai_chat_backend.h"
#include "agentflow/net/https_client.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "agentflow/workflow/workflow_runner.h"
#include "examples/deep-search/tavily_tools.h"
#include "test_messages.pb.h"

namespace af = agentflow;

namespace {

std::string RequiredEnv(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    std::cerr << "missing required environment variable: " << name << "\n";
    std::exit(2);
  }
  return v;
}

std::string OptionalEnv(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string question =
      argc > 1 ? argv[1] : "What is the current state of on-device LLM "
                           "inference?";
  const std::string tavily_key = RequiredEnv("TAVILY_API_KEY");

  asio::io_context io;

  // HTTPS client is needed in BOTH modes (Tavily tools).
  af::net::HttpsClientOptions http_opts;
  http_opts.ca_path = OptionalEnv("AGENTFLOW_LLM_CA_PATH");
  if (http_opts.ca_path.empty()) {
    http_opts.ca_path = "/etc/ssl/certs/ca-certificates.crt";
  }
  af::net::HttpsClient http(io, http_opts);

  // Backend under the logical name "cloud" (what workflow.json names).
  std::shared_ptr<af::IChatBackend> cloud;
  std::string mode;
  const std::string model_path = OptionalEnv("MODEL_PATH");
  if (!model_path.empty()) {
    auto engine = af::LiteRtLmEngine::Create(
        af::LiteRtLmEngineOptions{.model_path = model_path});
    if (!engine) {
      std::cerr << "failed to create LiteRT-LM engine\n";
      return 1;
    }
    cloud = af::LiteRtLmChatBackend::Create(engine, io);
    mode = "local:" + model_path;
  } else {
    af::openai::OpenAiOptions llm;
    llm.base_url = RequiredEnv("AGENTFLOW_LLM_BASE_URL");
    llm.model = RequiredEnv("AGENTFLOW_LLM_MODEL");
    llm.api_key = OptionalEnv("AGENTFLOW_LLM_API_KEY");
    cloud = af::openai::OpenAiChatBackend::Create(llm, http);
    mode = "cloud:" + llm.model;
  }

  // Tools: Tavily search + extract, registered host-side.
  auto tools = std::make_shared<af::ToolRegistry>();
  tools->Register(deep_search::MakeTavilySearchTool(http, tavily_key));
  tools->Register(deep_search::MakeTavilyExtractTool(http, tavily_key));

  auto wf_or = af::workflow::WorkflowLoader::LoadFromFile(
      "examples/deep-search/workflow.json", *tools);
  if (!wf_or.ok()) {
    std::cerr << "failed to load workflow: " << wf_or.status().message()
              << "\n";
    return 1;
  }

  af::workflow::AgentNodeBuildSpec spec;
  spec.workflow = *wf_or;
  spec.agent_name = "deep_search";
  spec.host_tools = tools;
  spec.io_ctx = &io;
  spec.backends["cloud"] = cloud;
  spec.max_iter = 12;

  auto built = af::workflow::BuildAgentNode(spec);
  built.cfg.stream_tokens = true;
  af::AgentNode node(std::move(built.cfg));

  af::CallbackEventEmitter emit(
      [](const af::proto::TraceEvent& e) {
        if (e.has_token()) std::cout << e.token().token() << std::flush;
      });

  af::test::TestState state;
  state.set_user_query(question);

  af::CancelSource cancel;
  auto start = std::chrono::steady_clock::now();
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<af::State> {
        co_return co_await node.Run(af::State::From(std::move(state)),
                                    cancel.Token(), emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  std::cout << "\n---\nmode=" << mode << "\nelapsed_ms=" << elapsed.count()
            << "\nanswer:\n"
            << out.As<af::test::TestState>().assistant_reply() << "\n";
  return 0;
}
```

- [ ] **Step 3: Add the binary target to `examples/deep-search/BUILD.bazel`**

```python
cc_binary(
    name = "deep_search",
    srcs = ["main.cc"],
    data = ["workflow.json"],
    deps = [
        ":tavily_tools",
        "//agentflow/core",
        "//agentflow/inference",
        "//agentflow/inference/openai:openai_chat_backend",
        "//agentflow/net:https_client",
        "//agentflow/nodes",
        "//agentflow/tools",
        "//agentflow/workflow:workflow_loader",
        "//agentflow/workflow:workflow_runner",
        "//proto:agentflow_proto",
        "@asio",
    ],
)
```

- [ ] **Step 4: Build**

Run: `bazel build //examples/deep-search:deep_search`
Expected: compile success.

- [ ] **Step 5: Commit**

```bash
git add examples/deep-search/workflow.json examples/deep-search/main.cc examples/deep-search/BUILD.bazel
git commit -m "feat(examples): deep-search host with cloud/local dual backend"
```

---

### Task 6: Real-cloud e2e test

**Files:**
- Create: `tests/integration/deep_search_e2e_test.cc`
- Modify: `tests/integration/BUILD.bazel` (add test target)

**Interfaces:**
- Consumes: everything from Task 4 and Task 5 (`tavily_tools`, `workflow.json` via `data`), `CallbackEventEmitter`, `proto::TraceEvent` fields `tool_call` (`ToolCallPayload.tool_name`) and `tool_return` (`ToolReturnPayload.tool_name`, `result_json`), `remote_llm_e2e_test.cc` pattern (env-gated `GTEST_SKIP`).
- Produces: `//tests/integration:deep_search_e2e_test` — opt-in, offline-skipping, structure-only assertions.

- [ ] **Step 1: Create `tests/integration/deep_search_e2e_test.cc`**

```cpp
// tests/integration/deep_search_e2e_test.cc
//
// Opt-in end-to-end check: the deep-search workflow against a real cloud
// endpoint with real Tavily traffic. Requires:
//   AGENTFLOW_LLM_BASE_URL, AGENTFLOW_LLM_MODEL, TAVILY_API_KEY
// Optional: AGENTFLOW_LLM_API_KEY, AGENTFLOW_LLM_CA_PATH
// Skipped by default so CI stays offline and free.
//
// Asserts STRUCTURE, never model prose:
//   - the run completes with a non-empty final answer,
//   - no delegate call returned an error placeholder,
//   - when the model issued >= 2 delegate calls, all delegate TOOL_CALL
//     events precede all delegate TOOL_RETURN events — the observable proof
//     they were spawned concurrently (a sequential dispatch loop would
//     interleave call/return pairs).
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/core/state.h"
#include "agentflow/inference/openai/openai_chat_backend.h"
#include "agentflow/net/https_client.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "agentflow/workflow/workflow_runner.h"
#include "examples/deep-search/tavily_tools.h"
#include "test_messages.pb.h"

namespace agentflow {
namespace {

TEST(DeepSearchE2ETest, RealCloudRunFansOutAndGathers) {
  const char* base = std::getenv("AGENTFLOW_LLM_BASE_URL");
  const char* model = std::getenv("AGENTFLOW_LLM_MODEL");
  const char* tavily = std::getenv("TAVILY_API_KEY");
  if (!base || !model || !tavily) {
    GTEST_SKIP() << "AGENTFLOW_LLM_BASE_URL / AGENTFLOW_LLM_MODEL / "
                    "TAVILY_API_KEY not set";
  }
  const char* key = std::getenv("AGENTFLOW_LLM_API_KEY");   // optional
  const char* ca = std::getenv("AGENTFLOW_LLM_CA_PATH");    // optional

  asio::io_context io;
  net::HttpsClientOptions http_opts;
  http_opts.ca_path = ca ? ca : "/etc/ssl/certs/ca-certificates.crt";
  net::HttpsClient http(io, http_opts);

  openai::OpenAiOptions opts;
  opts.base_url = base;
  opts.model = model;
  if (key) opts.api_key = key;
  auto cloud = openai::OpenAiChatBackend::Create(opts, http);

  auto tools = std::make_shared<ToolRegistry>();
  tools->Register(deep_search::MakeTavilySearchTool(http, tavily));
  tools->Register(deep_search::MakeTavilyExtractTool(http, tavily));

  auto wf_or = workflow::WorkflowLoader::LoadFromFile(
      "examples/deep-search/workflow.json", *tools);
  ASSERT_TRUE(wf_or.ok()) << wf_or.status().message();

  workflow::AgentNodeBuildSpec spec;
  spec.workflow = *wf_or;
  spec.agent_name = "deep_search";
  spec.host_tools = tools;
  spec.io_ctx = &io;
  spec.backends["cloud"] = cloud;
  spec.max_iter = 12;

  auto built = workflow::BuildAgentNode(spec);
  AgentNode node(std::move(built.cfg));

  // Capture tool-call / tool-return events for the delegate tool.
  std::mutex mu;
  std::vector<proto::TraceEvent> events;
  CallbackEventEmitter emit([&](const proto::TraceEvent& e) {
    std::lock_guard<std::mutex> l(mu);
    events.push_back(e);
  });

  test::TestState state;
  state.set_user_query("Who won the last Formula 1 race?");

  CancelSource cancel;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node.Run(State::From(std::move(state)),
                                    cancel.Token(), emit);
      },
      asio::use_future);
  io.run();
  State out = fut.get();

  // Structure assertion 1: a non-empty final answer was written.
  const std::string answer = out.As<test::TestState>().assistant_reply();
  EXPECT_FALSE(answer.empty()) << "final answer must be non-empty";

  // Collect delegate events in order.
  std::lock_guard<std::mutex> l(mu);
  std::vector<size_t> call_pos;
  std::vector<size_t> ret_pos;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].has_tool_call() &&
        events[i].tool_call().tool_name() == "delegate") {
      call_pos.push_back(i);
    }
    if (events[i].has_tool_return() &&
        events[i].tool_return().tool_name() == "delegate") {
      // Structure assertion 2: no error placeholder from any sub-agent.
      EXPECT_FALSE(events[i].tool_return().result_json().find("\"error\"") !=
                   std::string::npos)
          << "delegate returned an error: "
          << events[i].tool_return().result_json();
      ret_pos.push_back(i);
    }
  }

  // Structure assertion 3: if the model fanned out (>= 2 delegate calls),
  // every TOOL_CALL precedes every TOOL_RETURN — all were spawned before any
  // completed, i.e. concurrent dispatch. Sequential dispatch would
  // interleave call/return pairs.
  if (call_pos.size() >= 2) {
    ASSERT_EQ(call_pos.size(), ret_pos.size());
    size_t last_call = *std::max_element(call_pos.begin(), call_pos.end());
    size_t first_ret = *std::min_element(ret_pos.begin(), ret_pos.end());
    EXPECT_LT(last_call, first_ret)
        << "delegate calls were not dispatched concurrently";
  } else {
    GTEST_SKIP() << "model emitted fewer than 2 delegate calls; "
                    "concurrency assertion not applicable";
  }
}

}  // namespace
}  // namespace agentflow
```

(If `std::max_element`/`std::min_element` are used, add `#include <algorithm>`.)

- [ ] **Step 2: Add the test target to `tests/integration/BUILD.bazel`**

```python
cc_test(
    name = "deep_search_e2e_test",
    size = "small",
    srcs = ["deep_search_e2e_test.cc"],
    data = ["//examples/deep-search:workflow.json"],
    deps = [
        "//agentflow/core",
        "//agentflow/inference/openai:openai_chat_backend",
        "//agentflow/net:https_client",
        "//agentflow/nodes",
        "//agentflow/tools",
        "//agentflow/workflow:workflow_loader",
        "//agentflow/workflow:workflow_runner",
        "//examples/deep-search:tavily_tools",
        "//proto:agentflow_proto",
        "@asio",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 3: Verify it skips without env**

Run: `bazel test //tests/integration:deep_search_e2e_test`
Expected: PASS with `GTEST_SKIP()` (no `AGENTFLOW_LLM_*` / `TAVILY_API_KEY` set).

- [ ] **Step 4: Real cloud run**

Run (with real credentials in the shell):
`TAVILY_API_KEY=tvly-... AGENTFLOW_LLM_BASE_URL=... AGENTFLOW_LLM_MODEL=... AGENTFLOW_LLM_API_KEY=... bazel test //tests/integration:deep_search_e2e_test --test_output=all`
Expected: PASS with real Tavily traffic; the log shows ≥2 delegate TOOL_CALL events before any TOOL_RETURN.

- [ ] **Step 5: Commit**

```bash
git add tests/integration/deep_search_e2e_test.cc tests/integration/BUILD.bazel
git commit -m "test(integration): deep-search real-cloud e2e with concurrency assertion"
```

---

### Task 7: Local gemma-4 wall-time comparison (manual, not CI)

**Files:** none (uses the Task 5 binary).

**Interfaces:**
- Consumes: `//examples/deep-search:deep_search` binary, `models/gemma-4-E2B-it.litertlm`, `TAVILY_API_KEY`.

- [ ] **Step 1: Run locally against gemma-4-E2B-it**

Run:
`MODEL_PATH=models/gemma-4-E2B-it.litertlm TAVILY_API_KEY=tvly-... bazel run //examples/deep-search:deep_search -- "Who won the last Formula 1 race?"`
Expected: run completes; note the printed `mode=local:...` and `elapsed_ms`.

- [ ] **Step 2: Run the same query against cloud**

Run:
`TAVILY_API_KEY=tvly-... AGENTFLOW_LLM_BASE_URL=... AGENTFLOW_LLM_MODEL=... AGENTFLOW_LLM_API_KEY=... bazel run //examples/deep-search:deep_search -- "Who won the last Formula 1 race?"`
Expected: run completes; note `mode=cloud:...` and `elapsed_ms`.

- [ ] **Step 3: Record the comparison**

Add a short `examples/deep-search/README.md` with the two commands and the observed numbers (fill in the real values after running), plus the observation whether gemma-4-E2B-it emitted multi-call turns (visible in the event log). Commit:

```bash
git add examples/deep-search/README.md
git commit -m "docs(examples): deep-search run commands and cloud/local wall-time notes"
```

---

## Self-Review Notes (for the executor)

- Spec coverage: §2 core semantics → Task 2; §3 contract → Tasks 1–2 tests; §4.2/4.3 wording → Task 3; §5 safety argument → no code change (argument only); §6.1–6.2 → Tasks 1–3; §6.3 cloud e2e → Task 6; §6.4 local comparison → Task 7; §7 deliverables → Tasks 4–5. Gap check: none.
- The concurrency assertion in Task 6 uses event ordering (all TOOL_CALL before all TOOL_RETURN) as the observable equivalent of the spec's "wall times overlap" — stronger and deterministic; keep it.
- Type consistency: tool names `tavily_search` / `tavily_extract` are used identically in Task 4 schemas, Task 5 `workflow.json` `tools` arrays, and Task 6 registration. Logical backend name `"cloud"` is used identically in `workflow.json`, Task 5 `main.cc`, and Task 6 spec.
- The `data = ["workflow.json"]` relative path `"examples/deep-search/workflow.json"` in `main.cc`/e2e test relies on bazel runfiles layout exactly as `examples/remote-llm/main.cc` does — do not "fix" it to an absolute path.
