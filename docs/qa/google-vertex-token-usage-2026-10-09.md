# Real Vertex AI token usage validation

On 2026-10-09, real streamed calls exercised Vertex AI's OpenAI-compatible endpoint using service-account OAuth, ZenAgent, Zen Chat's signed usage callback, Mirashot FastAPI and an isolated PostgreSQL test account. A transparent local relay retained provider SSE without replacing model responses. The prompt asked for `17 * 23`; all calls returned `391` and requested `stream_options.include_usage=true`.

| Run | Model | Prompt | Completion | Reasoning | Provider total | Ledger input/output/total |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| Before fix | google/gemini-2.5-flash | 26 | 3 | 84 | 113 | 26 / 3 / 29 |
| After fix | google/gemini-2.5-flash | 26 | 3 | 241 | 270 | 26 / 244 / 270 |
| After fix | google/gemini-3.1-pro-preview | 26 | 3 | 162 | 191 | 26 / 165 / 191 |

The pre-fix ledger omitted Google's thinking tokens. Google reports thinking separately from `completion_tokens`; the earlier real DeepSeek call reported 40 prompt, 11 completion including 9 reasoning, and 51 total. Adding reasoning blindly would double count DeepSeek. The mapper and streaming accumulator now use `total_tokens - prompt_tokens` when all counts are valid and that difference is at least the completion count. Missing, malformed, overflowing or inconsistent totals preserve the validated completion count.

Both post-fix ledger rows were compared for exact equality with the corresponding provider's input, output and total. Usage appeared on a choices-bearing final frame, also covering that provider behavior.

Regression tests failed on Google's undercount before the fix. After the fix, 69 cases across these three Bazel targets passed, and the companion service binary built successfully:

```sh
bazel test @agentflow//tests/unit/inference/openai:message_map_test \
  @agentflow//tests/unit/inference/openai:stream_accumulator_test \
  @agentflow//tests/unit/inference/openai:openai_chat_backend_test --test_output=errors
bazel build //server:zen_chat_service
```

Run these commands from the companion Zen Chat checkout with its local AgentFlow override. Tests cover Google thinking, DeepSeek reasoning without double counting, and invalid or inconsistent totals in streaming and nonstreaming mapping. Independent review found no actionable issue.

[Sanitized machine-readable evidence](google-vertex-token-usage-2026-10-09.json) contains only model names, token counts and verification results. Credentials, OAuth tokens, personal data and opaque thought signatures are omitted.

Scope: the two named models were called live through the streaming compatibility API; nonstream behavior was regression-tested. The callback used a local test identity, rather than the full browser authentication chain. Only an isolated local stack was used, with no shared-stack or production changes.
