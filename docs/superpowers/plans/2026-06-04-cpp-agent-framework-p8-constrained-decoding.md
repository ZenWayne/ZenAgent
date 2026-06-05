# P8 — Constrained decoding via LLGuidance C bridge

**Branch:** `feat/p8-constrained-decoding` (stacked on `feat/p7-checkpoint`)
**Spec anchor:** §1.4 feature #10 (P1, "Constrained decoding (依赖 LiteRT-LM 原生支持)").
**Goal:** Make `AgentNodeConfig::constrained_tool_calls = true` actually force the model's tool-call output to match the registered tools' parameter schemas. The LiteRT-LM C ABI doesn't expose its working constrained-decoding path (only a boolean that routes to a stubbed Gemma provider), so this phase **adds a minimal C bridge inside LiteRT-LM/c/engine.{h,cc}** that exposes the LLGuidance path through the same ABI.

## Why the standard C API isn't enough

LiteRT-LM has TWO constraint paths internally (see `runtime/conversation/conversation.cc:270` `CreateDecodeConfig`):

- **Path 1 (LLGuidance) — works.** Activated by `ConversationConfig::Builder::SetConstraintProviderConfig(LlGuidanceConfig{})` + per-message `OptionalArgs.decoding_constraint = ConstraintArg{LlGuidanceConstraintArg{kLark, grammar}}`. Real implementation: `runtime/components/constrained_decoding/llg_constraint_provider.cc` + LLGuidance Rust crate. Symbols are present in our `libce_*.a`.
- **Path 2 (Gemma-specific) — stubbed.** Activated by `Builder::SetEnableConstrainedDecoding(true)`. The actual provider source `gemma_model_constraint_provider.cc` is NOT in the open-source LiteRT-LM repo; CMake substitutes `cmake/patches/stubs/gemma_model_constraint_provider.cc` which returns `nullptr`.

The standard C engine API (`c/engine.h`) only exposes the Path 2 boolean. Path 1 — the one that actually works — needs `SetConstraintProviderConfig` + `ConstraintArg`, neither of which the C ABI surfaces.

## Design

Add two new C functions to `LiteRT-LM/c/engine.{h,cc}`:

```c
LiteRtLmConversation* litert_lm_engine_create_constrained_conversation(
    LiteRtLmEngine* engine,
    const char* system_message_json,
    const char* tools_json);

LiteRtLmJsonResponse* litert_lm_conversation_send_message_constrained(
    LiteRtLmConversation* conversation,
    const char* message_json);
```

Internally:
- `create_constrained_conversation` builds a `ConversationConfig` with `SetConstraintProviderConfig(LlGuidanceConfig{})`, then pre-builds a Lark grammar from `tools_json` via `CreateLarkGrammarForTools(...)` and stores it on the conversation.
- `send_message_constrained` attaches the stored grammar as `OptionalArgs.decoding_constraint = LlGuidanceConstraintArg{kLark, grammar}` on every call.

The existing `LiteRtLmConversation` struct grows a `std::string lark_grammar` field; existing constructors leave it empty so they keep working unchanged.

Agentflow consumes the new C functions through the existing wrapper pattern: `LiteRtLmConversation::Create` branches on `opts.constrained_tool_calls` and routes to the constrained C factory; `SendMessageSync` branches similarly. `AgentNodeConfig::constrained_tool_calls` propagates through `LiteRtLmConversationOptions`.

## File Structure

```
LiteRT-LM/
  c/
    engine.h                                     # MODIFY: 2 new function decls
    engine.cc                                    # MODIFY: 2 new function impls + lark_grammar field on the C struct
third_party/litert_lm/
  include/c/engine.h                              # MODIFY: synced copy
  lib/libce_staging.a                             # REGEN: re-merged after CMake rebuild
agentflow/
  inference/litert_lm_conversation.{h,cc}        # MODIFY: route Create/SendMessageSync to new C funcs when constrained
  nodes/agent_node.{h,cc}                        # MODIFY: AgentNodeConfig::constrained_tool_calls + propagate
examples/agent-demo/main.cc                       # MODIFY: enable the flag
tests/unit/nodes/agent_node_test.cc               # ADD: ConstrainedToolCallsHasAllRequiredKeys
docs/superpowers/plans/
  2026-06-04-cpp-agent-framework-p8-constrained-decoding.md   # this file
```

## Task Dependency

```
T1 (C bridge in engine.h/cc) → T2 (CMake rebuild) → T3 (re-merge libce_staging.a) → T4 (agentflow wrapper) → T5 (AgentNode + test + demo) → T6 (PR)
```

## Task 1 — extern C functions in LiteRT-LM/c/engine.{h,cc}

- Declarations at the end of the `extern "C"` block in `engine.h`. Document the relationship to Path 1/Path 2 clearly.
- Implementation in `engine.cc`:
  - Add to includes: `runtime/components/constrained_decoding/{constraint_provider_config,llg_constraint_config,llguidance_schema_utils}.h`.
  - Extend `struct LiteRtLmConversation` with `std::string lark_grammar` (file-private struct in the .cc, existing consumers unaffected).
  - `create_constrained_conversation`: build `JsonPreface` exactly like `litert_lm_conversation_config_create`, set `LlGuidanceConfig{}` on the builder, build conversation. Pre-build Lark grammar via `CreateLarkGrammarForTools(tools, opts{kFunctionCallsOnly})`; log + leave empty on failure.
  - `send_message_constrained`: parse `message_json`, attach `OptionalArgs.decoding_constraint = LlGuidanceConstraintArg{kLark, grammar}` if grammar non-empty, call `SendMessage` (sync — avoids the broken stream path).

Commit: `feat(p8): LiteRT-LM C bridge for LLGuidance constrained conversation`

## Task 2 — rebuild libce_*.a via CMake

- Copy updated `engine.cc`/`engine.h` into `.litert_build/litert_lm/build/generated/src/c/` (CMake stages source files there).
- `cmake --build .litert_build/litert_lm/build --target c_engine -j4`.
- Verify symbols: `nm -C .litert_build/.../staging/lib/libc_engine.a | grep constrained` should show both new functions.

## Task 3 — re-merge new symbols into `third_party/litert_lm/lib/libce_staging.a`

- `ar -r third_party/litert_lm/lib/libce_staging.a .litert_build/.../engine.cc.o` (replaces just the `engine.cc.o` member in-place).
- Verify with `nm -C` that `litert_lm_conversation_send_message_constrained` is now visible from `libce_staging.a`.
- No other archive needs updating — `libce_external.a` has the LLGuidance crate; the new bridge only added C++ logic in `engine.cc.o`.

Commit (T1+T2+T3 logically bundled, since they only matter together): `feat(p8): rebuilt libce_staging.a with constrained conversation C bridge`

## Task 4 — agentflow wrapper

- Sync `third_party/litert_lm/include/c/engine.h` to match the LiteRT-LM source copy (so callers see the new decls).
- `LiteRtLmConversationOptions`: drop `enable_constrained_decoding`, add `constrained_tool_calls`.
- `LiteRtLmConversation`: add `bool constrained_` member; `Create` branches on `opts.constrained_tool_calls && tools != nullptr` to the new C factory; `SendMessageSync` routes to `litert_lm_conversation_send_message_constrained` when `constrained_` is true.

Commit: `feat(p8): wrap C bridge in LiteRtLmConversation`

## Task 5 — AgentNode + test + demo

- `AgentNodeConfig::constrained_tool_calls = false` (new field).
- `AgentNode::Run`: pass through to `LiteRtLmConversationOptions::constrained_tool_calls`.
- New MODEL_PATH-gated test `ConstrainedToolCallsHasAllRequiredKeys`: weather tool with `required: [location, units]`, prompt that elides `units`, assert tool args contain both keys after the constrained call.
- `examples/agent-demo/main.cc`: `agent_cfg.constrained_tool_calls = true`.

Commit: `feat(p8): AgentNodeConfig.constrained_tool_calls; tests + demo`

## Task 6 — Force-push PR #9

- Plan doc reflects the C bridge path.
- Tag `p8-constrained-decoding`.
- Force-push branch.
- PR body: explain the two-path discovery, why C bridge over Path B, the tiny upstream change.

## Verification

- `bazel test //...` — 20/20 hermetic tests pass.
- `MODEL_PATH=... bazel test //tests/unit/nodes:agent_node_test` — 3/3 (RealModelReturnsNonEmptyReply, UsesGetTimeTool, ConstrainedToolCallsHasAllRequiredKeys) pass.
- `MODEL_PATH=... bazel run //examples/agent-demo:agent_demo` — coherent answer with timestamp; tool call args constrained to schema.

## Risks + Out-of-scope

- **Risk:** The lark_grammar field on the C struct breaks ABI compat if anyone else included `struct LiteRtLmConversation`. They can't — the struct is file-private to `c/engine.cc` (opaque to C consumers).
- **Risk:** Upstream LiteRT-LM may eventually add similar C functions with different names. Rebase will need to drop our bridge in favor of theirs.
- **Out of scope:** Per-message arbitrary regex/JSON-schema constraints (would need exposing more of `ConstraintArg` via C). Lark grammar auto-derived from tools is sufficient for v1's "constrained tool calls" use case.
- **Out of scope:** LlmNode constrained decoding (uses Session, not Conversation — would need its own C bridge or a Session refactor).

## Execution Handoff

Each task ends in a commit. T2/T3 produce binary artifacts (the rebuilt `libce_staging.a`) that have to be committed — they're checked into git so Bazel builds don't have to re-run CMake.
