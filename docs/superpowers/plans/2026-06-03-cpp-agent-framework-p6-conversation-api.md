# P6 — LiteRT-LM Conversation API: proper chat template + tool-call parsing

**Branch:** `feat/p6-conversation`
**Goal:** Replace our raw Session-based prompting with LiteRT-LM's structured Conversation API so the model receives a properly templated chat (system + tools + history) and the engine returns structured JSON the framework can parse without guessing at Gemma's `<|tool_call>...<tool_call|>` markers.

P5 closed the trace channel. P6 closes the "we're shouting raw JSON at a chat model" gap that's making agent-demo emit `],"name":"assistant"}]}}` instead of real answers. The inference path is *working* (Prefill OK, Decode streams tokens, AgentNode receives them). The model just sees a garbled prompt because we never apply its chat template.

## What's wrong today

`AgentNode::BuildConversationJson` dumps a single JSON object:
```json
{"messages":[...],"max_tokens":1024,"stream":true,"tools":[...]}
```
…which it then passes verbatim to `LiteRtLmSession::Start(text)`. The session treats that string as raw model input. Gemma's tokenizer never sees `<|turn>system`, `<|tool>...<tool|>`, `<|turn>user`, `<|turn>model` markers — it just starts continuing the literal JSON. Hence the garbage tail in the response.

Even if a real reply arrived, `AgentNode::Run` matches `accum` against `{"tool_calls":[{"id":...,"function":{"name":...,"arguments":...}}]}` (OpenAI shape), but Gemma emits `<|tool_call>call:name{k:v}<tool_call|>` per its template. The parse always misses → AgentNode never dispatches tools → the "final answer" fallback prints the garbage.

## What we have available

`third_party/litert_lm/include/c/engine.h` exposes a second tier of APIs above Session:

```c
LiteRtLmConversationConfig* litert_lm_conversation_config_create(
    engine, session_config,
    const char* system_message_json,
    const char* tools_json,
    const char* messages_json,
    bool enable_constrained_decoding);

LiteRtLmConversation* litert_lm_conversation_create(engine, config);
int litert_lm_conversation_send_message_stream(
    conversation, const char* message_json, const char* extra_context,
    LiteRtLmStreamCallback cb, void* cb_data);
void litert_lm_conversation_cancel_process(conversation);
```

The engine's config dump confirms `use_template_for_fc_format: true`, and the model ships with `jinja_prompt_template` baked into its `.litertlm` file. Using Conversation lets the engine apply that template and parse tool calls back into structured JSON (the response type is `LiteRtLmJsonResponse`, accessed via `litert_lm_json_response_get_string`).

## File Structure

```
agentflow/
  inference/
    litert_lm_conversation.{h,cc}        # NEW — C++ wrapper for the Conversation API
    litert_lm_session.{h,cc}             # KEEP — LlmNode users who want raw Session can still use it
  nodes/
    agent_node.{h,cc}                    # MODIFY — Run loop uses Conversation, drops BuildConversationJson
    llm_node.{h,cc}                      # MODIFY — same, single-shot variant
examples/
  agent-demo/main.cc                     # MODIFY — verify get_time tool returns a real time
tests/unit/
  nodes/
    agent_node_test.cc                   # MODIFY — DISABLED → MODEL_PATH-gated real-model test
    llm_node_test.cc                     # MODIFY — same
docs/superpowers/plans/
  2026-06-03-cpp-agent-framework-p6-conversation-api.md   # this file
```

## Task Dependency

```
T1 (Conversation wrapper) ──→ T2 (probe response shape)
                              │
                              ├──→ T3 (LlmNode refactor) ──→ T5 (tests)
                              └──→ T4 (AgentNode refactor) ─┘
                                                            ▼
                                                          T6 (demo) ──→ T7 (tag + PR)
```

## Task 1: `LiteRtLmConversation` wrapper

**Files:** `agentflow/inference/litert_lm_conversation.{h,cc}`

### Step 1.1: Header

```cpp
namespace agentflow {

struct LiteRtLmConversationOptions {
  std::string system_message_json;   // e.g. {"role":"system","content":"..."}
  std::string tools_json;            // e.g. [{"type":"function","function":{...}}, ...]  ([] for none)
  std::string messages_json;         // initial history ([] for fresh)
  bool enable_constrained_decoding = false;
  int max_output_tokens = 1024;
};

class LiteRtLmConversation {
 public:
  static std::shared_ptr<LiteRtLmConversation> Create(
      std::shared_ptr<LiteRtLmEngine> engine,
      LiteRtLmConversationOptions opts,
      asio::io_context& io_ctx);

  ~LiteRtLmConversation();

  // Streams response chunks. Each token call resolves NextTokenAsync exactly
  // like LiteRtLmSession. After the final chunk, NextTokenAsync returns an
  // empty string. The model's full JSON response (with parsed tool_calls,
  // if any) is also recoverable via `FullResponseJson()` after streaming
  // completes.
  void SendMessage(std::string message_json, std::string extra_context = "");
  asio::awaitable<std::string> NextTokenAsync();
  std::string FullResponseJson() const;  // populated when stream completes
  void Cancel();

 private:
  /* opaque LiteRtLmConversation*, asio channel, io_ctx ref, accum buffer */
};
}
```

Implementation mirrors `LiteRtLmSession` — `asio::experimental::channel<void(asio::error_code, std::string)>`, callback `asio::post`s tokens onto io_ctx (LiteRT calls back from its worker thread; the channel isn't thread-safe).

### Step 1.2: BUILD wiring + commit

Add to `agentflow/inference/BUILD.bazel`'s existing target. Build alone first to verify the headers link.

Commit: `feat: LiteRtLmConversation wrapper around C engine conversation API`

## Task 2: Probe the actual response shape — RESOLVED

**Findings (2026-06-04):**

1. **Streaming variant `litert_lm_conversation_send_message_stream` is broken in our linkage.** Every call fails inside `prompt_template_.Apply` with `Failed to apply template: expected value at line 1 column N`, where N tracks our input size. Repro is consistent regardless of preface contents, with/without tools, with plain-string or typed-content messages. `litert_lm_main` (which uses the C++ API directly + `engine->WaitUntilDone`) works on the same model — suggests a thread-context interaction in our archive's minijinja invocation that only manifests on the async-callback path.

2. **Non-streaming `litert_lm_conversation_send_message` works perfectly.** Same options, same prompt:
   ```
   bazel run //tmp_probe:probe → "Hello! How can I help you today? 😊"
   ```

3. **Response shape (locked in):**
   ```json
   {"role":"assistant","content":[{"type":"text","text":"..."}]}
   ```
   For tool calls the assistant message also includes `tool_calls` (shape per LiteRT-LM's standard).

4. **System message takes typed-content shape**, not OpenAI `{role,content}` envelope:
   ```json
   {"type":"text","text":"You are a helpful assistant."}
   ```
   The engine wraps it into `{role:system, content:...}` internally.

5. **User messages**: `{"role":"user","content":[{"type":"text","text":"..."}]}` (content is array of typed items).

**Pivot for T3/T4:** Add a `SendMessageSync` method to `LiteRtLmConversation` that wraps the non-streaming C API and returns the full response JSON. Token streaming on the Conversation path is unavailable; emit one `EmitToken` with the full text after the call returns. Streaming still works via the older `LiteRtLmSession` for callers that don't need tool dispatch (LlmNode's raw single-shot path).

Commit (plan update): `docs: P6 T2 findings — Conversation stream variant broken, use sync path`

## Task 3: Refactor `LlmNode` to use Conversation

**Files:** `agentflow/nodes/llm_node.{h,cc}`

### Step 3.1: Replace `BuildConversationJson` and `session.Start`

- Build `system_message_json` from `cfg_.system_prompt`.
- Build `tools_json` from `cfg_.tool_registry->ExportToolsJson(cfg_.tool_names)` (when registry present, else `"[]"`).
- Build `messages_json` empty for now (single-shot; no prior history). Build the user `message_json` from `cfg_.input_field`.
- `auto conv = LiteRtLmConversation::Create(engine, opts, io_ctx);`
- `conv->SendMessage(message_json);`
- Drain `NextTokenAsync()` into `accum`, emitting `EmitToken` per chunk.
- Write `conv->FullResponseJson()` to `cfg_.output_field` (LlmNode is a single-call node per spec §5.1; structured tool dispatch is AgentNode's job).

Keep the `LiteRtLmSession`-based path? **No** — single-shot LlmNode users still benefit from the chat template. The Session API stays as the wrapper for advanced callers who want raw prompts.

### Step 3.2: Commit

`refactor: LlmNode uses LiteRtLmConversation (chat template + tool schema applied by engine)`

## Task 4: Refactor `AgentNode` to use Conversation across the ReAct loop

**Files:** `agentflow/nodes/agent_node.{h,cc}`

### Step 4.1: Lifecycle change

A Conversation persists across messages; the engine tracks history internally. AgentNode's ReAct loop becomes:

```cpp
auto conv = LiteRtLmConversation::Create(engine, /*system+tools+empty history*/, io_ctx);
conv->SendMessage(user_message_json);

for (int iter = 0; iter < cfg_.max_iter; ++iter) {
  std::string accum;
  while (true) {
    auto tok = co_await conv->NextTokenAsync();
    if (tok.empty()) break;
    accum += tok;
    if (cfg_.stream_tokens) emit.EmitToken(Id(), tok);
  }

  auto resp = json::parse(conv->FullResponseJson());
  // Schema documented in T2. Branch:
  if (resp has tool_calls) {
    for (each call) {
      emit.EmitToolCall(Id(), name, args);
      auto result = co_await cfg_.tool_registry->Invoke(name, args, cancel);
      emit.EmitToolReturn(Id(), name, result);
      conv->SendMessage(MakeToolResponseJson(call_id, name, result));
    }
    continue;  // engine appends history + decodes next turn
  } else {
    WriteOutput(state, accum_or_resp_text);
    break;
  }
}
```

### Step 4.2: Drop dead code

- `BuildConversationJson` (no longer needed — engine does it).
- `ReadMessages` (engine owns history).
- `AppendMessage` (engine owns history).
- `cfg_.messages_field` becomes a no-op; deprecate via a doc comment "// reserved — engine manages history internally as of P6".

### Step 4.3: Commit

`refactor: AgentNode drives ReAct via Conversation; engine owns message history`

## Task 5: Tests

### Step 5.1: Integration tests gated on `MODEL_PATH`

`tests/unit/nodes/agent_node_test.cc`:
- Existing `DISABLED_SimpleResponse` already requires `MODEL_PATH`. **Un-DISABLE** it; rename `AgentNodeIntegrationTest.RealModelAnswersHello` (still skipped when `MODEL_PATH` unset via `GTEST_SKIP()`).
- New `AgentNodeIntegrationTest.UsesGetTimeTool` — registers the get_time tool, prompts "What time is it?", asserts:
  - the assistant reply is non-empty
  - at least one TOOL_CALL event was emitted with `tool_name == "get_time"`
  - the reply contains a parseable time substring (regex `\d{4}` matches a year or seconds).

`tests/unit/nodes/llm_node_test.cc`:
- Symmetric `LlmNodeIntegrationTest.RealModelReturnsText` — single SendMessage, assert non-empty reply.

### Step 5.2: Hermetic unit tests

`AgentNode` no longer holds the message_json builder, so we can't unit-test that without engine. **Acceptable trade-off**: AgentNode becomes thinner. Existing AgentNode unit-test coverage that didn't need a model still applies (ctor validation, etc.).

### Step 5.3: Commit

`test: gate Llm/Agent integration tests on MODEL_PATH; add get_time tool dispatch test`

## Task 6: Demo verification

`examples/agent-demo/main.cc`:
- Already wires get_time tool. After T4, re-run with `MODEL_PATH=$(realpath models/gemma-4-E2B-it.litertlm)`.
- Expected: assistant reply contains a real timestamp string. Trace shows `TOOL_CALL{tool_name=get_time}` → `TOOL_RETURN{result_json=<time>}` → `TOKEN`s → final answer.
- Compare against P5-era output of `Assistant: ],"name":"assistant"}]}}` to confirm the fix.

Commit (only if main.cc needed touch-ups): `examples: agent-demo verified end-to-end with get_time tool`

## Task 7: Tag + PR

- `git tag p6-conversation`
- Push branch + open PR against master.
- PR title: `feat(p6): use LiteRT-LM Conversation API for chat template + tool parsing`
- PR body lists the two bugs closed (chat template not applied; OpenAI vs Gemma tool-call shape mismatch), the new wrapper class, and the verified demo output.

Commit: `chore: tag p6-conversation`

## Self-Review

**Why not just hand-template inside AgentNode?**
We'd need to ship a jinja runtime in C++ and keep the template in sync with the model. The engine already does this and tracks it per-model in the `.litertlm` file. Using its own API is strictly cheaper.

**Why isn't this a single PR with P5?**
P5 was scoped to observability; P6 is scoped to inference correctness. They're orthogonal — landing P5 already gave us the JSONL trace that made this gap obvious in the first place.

**Risks:**
- The Conversation API may not be fully wired in our linked `libce_*.a` archives. T2 is the probe — if `litert_lm_conversation_create` returns NULL or segfaults, the plan stops and we fall back to manual templating in AgentNode (uglier but viable).
- Response JSON schema (T2 output) drives the AgentNode parsing logic. If LiteRT-LM changes the schema in a future version, AgentNode breaks. Pin LiteRT-LM via the submodule SHA (already done).
- Conversation owns history; if a user mutates `state.messages_field` between iterations, those changes are silently dropped. T4 step 4.2's "no-op" comment must be loud.

**Out of scope (YAGNI):**
- Multi-modal inputs (`kInputImage`, `kInputAudio` types).
- Conversation persistence/checkpointing (P8 territory if needed).
- Streaming partial tool calls (engine returns whole tool-call objects; streaming applies to assistant text only).

## Execution Handoff

Resume by reading this file and Task 1. Each task ends in a commit; do not batch. T2's output (response-shape addendum) is the unblock for T3/T4 — if shape is surprising, revisit the plan before writing code.
