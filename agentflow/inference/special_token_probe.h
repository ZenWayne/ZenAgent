// agentflow/inference/special_token_probe.h
//
// Generic discovery of a LiteRT-LM model's registered special tokens, so the
// unconstrained decode path can strip them wherever they leak into output
// (the observed case: gemma4's quote token <|"|> leaking inside tool-call
// arguments).
//
// A special token is a TOKENIZER property, not a template property: the string
// becomes a single atomic token only if the model's tokenizer registers it as
// an added token. This mirrors the engine's own model-type inference
// (LiteRT-LM runtime/util/model_type_utils.cc InferLlmModelType): infer the
// model family from the tokenizer, then verify the family's known candidates
// against the tokenizer one by one.
//
// Candidates are SYNTACTIC names (e.g. "<escape>" — the function-calling quote
// fence). What actually appears in raw output is the DETOKENIZED text of the
// registered token (e.g. "<|\"|>" on gemma4), so each verified candidate is
// resolved through detokenize and that resolved text is what gets stripped.
// The framework never guesses a leak string; the tokenizer tells it.
#ifndef AGENTFLOW_INFERENCE_SPECIAL_TOKEN_PROBE_H_
#define AGENTFLOW_INFERENCE_SPECIAL_TOKEN_PROBE_H_

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentflow {

// The special-token strings a model's tokenizer registers as added tokens, in
// detokenized form — exactly the text that leaks into raw output.
struct ModelSpecialTokens {
  std::vector<std::string> strip;

  bool empty() const { return strip.empty(); }
};

// Model family the host assigns when wiring a model. Mirrors the engine's own
// model-type notion (LiteRT-LM runtime/util/model_type_utils.cc): the family
// selects the set of special-token strings that may leak.
enum class ModelFamily {
  kUnknown,  // no decode — a model we know nothing about is never touched
  kGemma,
  kQwen,
};

// Tokenizer access shim, injectable for tests. Both callbacks return nullopt
// on error.
struct TokenizerProbe {
  std::function<std::optional<std::vector<int>>(std::string_view)>
      tokenize;
  std::function<std::optional<std::string>(const std::vector<int>&)>
      detokenize;
};

// The family's canonical special-token strings, in the form they appear
// leaked in raw output. Data, not logic: this mirrors the engine's built-in
// template registry (model_type_utils.cc) — one entry per family, extended
// when a new model family is wired.
ModelSpecialTokens FamilySpecialTokens(ModelFamily family);

// Infers the model family from the tokenizer and returns the model's
// registered special tokens in detokenized form. Unknown families yield an
// empty set. Requires the engine C tokenizer API (available from the
// 90f42140-era archives onward).
ModelSpecialTokens ProbeModelSpecialTokens(const TokenizerProbe& probe);

// Resolves the effective token set with precedence:
//   1. tokenizer probe (non-null `probe` + non-empty result) — exact,
//      tokenizer-verified detokenized strings;
//   2. host-declared family registry — the pre-upgrade path, when the
//      archives do not export the tokenizer API.
ModelSpecialTokens ResolveSpecialTokens(const TokenizerProbe* probe,
                                        ModelFamily declared_family);

}  // namespace agentflow

#endif  // AGENTFLOW_INFERENCE_SPECIAL_TOKEN_PROBE_H_
