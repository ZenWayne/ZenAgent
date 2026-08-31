// agentflow/inference/special_token_probe.cc
#include "agentflow/inference/special_token_probe.h"

#include <algorithm>
#include <span>
#include <utility>

namespace agentflow {
namespace {

// Special-token candidates per model family, as SYNTACTIC names from the
// family's built-in chat template and data-processor config. Mirrors the
// strings in LiteRT-LM's model_type_utils.cc GetDefaultJinjaPromptTemplate
// and function_gemma_data_processor_config.h (fence/escape must match the
// template). A candidate is only kept when this model's tokenizer registers
// it as a single token.
//
// "<escape>" is the function-calling quote fence; its detokenized form is the
// model's quote token (gemma4: <|"|>), which is what leaks in practice.
constexpr std::string_view kGemmaCandidates[] = {
    "<escape>",             // quote fence -> detokenizes to the quote token
    "<start_of_turn>", "<end_of_turn>",
    "<start_function_call>", "<end_function_call>",
    "<start_function_declaration>", "<end_function_declaration>",
    "<start_function_response>", "<end_function_response>",
    "<start_of_image>", "<end_of_image>",
    "<start_of_audio>", "<end_of_audio>",
};

constexpr std::string_view kQwenCandidates[] = {
    "<|im_start|>", "<|im_end|>",
    "<tool_call>", "</tool_call>",
};

bool TokenizesToSingleId(const TokenizerProbe& probe, std::string_view text,
                         int* id_out) {
  auto ids = probe.tokenize(text);
  if (!ids || ids->size() != 1) return false;
  *id_out = (*ids)[0];
  return true;
}

// Detokenized text for one candidate; nullopt when the candidate is not a
// registered single token in this model.
std::optional<std::string> ResolveCandidate(const TokenizerProbe& probe,
                                            std::string_view candidate) {
  int id = -1;
  if (!TokenizesToSingleId(probe, candidate, &id)) return std::nullopt;
  auto text = probe.detokenize({id});
  if (!text || text->empty()) return std::nullopt;
  return text;
}

void AddIfNew(ModelSpecialTokens& tokens, std::string text) {
  if (std::find(tokens.strip.begin(), tokens.strip.end(), text) ==
      tokens.strip.end()) {
    tokens.strip.push_back(std::move(text));
  }
}

}  // namespace

ModelSpecialTokens ProbeModelSpecialTokens(const TokenizerProbe& probe) {
  ModelSpecialTokens tokens;

  // Family inference, mirroring LiteRT-LM's InferLlmModelType: gemma
  // tokenizers detokenize id 1 to "<start_of_turn>"; qwen tokenizers register
  // "<|im_start|>" as a single token. Anything else is generic: return an
  // empty set rather than strip strings that may be ordinary text.
  std::span<const std::string_view> candidates;
  auto start_text = probe.detokenize({1});
  if (start_text && *start_text == "<start_of_turn>") {
    candidates = kGemmaCandidates;
  } else {
    int unused = -1;
    if (TokenizesToSingleId(probe, "<|im_start|>", &unused)) {
      candidates = kQwenCandidates;
    } else {
      return tokens;
    }
  }

  for (const std::string_view candidate : candidates) {
    auto resolved = ResolveCandidate(probe, candidate);
    if (resolved) AddIfNew(tokens, std::move(*resolved));
  }
  return tokens;
}

// The family's canonical leak strings, as they appear in raw decoded output.
// For gemma these are the chat-template control tokens (the quote fence
// <|"|> and the tool-call/turn markers); for qwen the <|im_*|> markers and
// tool-call fences. A new family is added alongside its template wiring.
ModelSpecialTokens FamilySpecialTokens(ModelFamily family) {
  ModelSpecialTokens tokens;
  switch (family) {
    case ModelFamily::kGemma:
      tokens.strip = {
          "<|\"|>",       // quote fence (the observed open_quote leak)
          "<|tool_call>", "</tool_call>", "<tool_call|>", "<|tool_call_ignore>",
          "<|tool_response>", "</tool_response>", "<tool_response|>",
          "<|image>", "<image|>", "<|audio>", "<audio|>",
          "<start_of_turn>", "<end_of_turn>",
          "<start_function_call>", "<end_function_call>",
          "<start_function_declaration>", "<end_function_declaration>",
          "<start_function_response>", "<end_function_response>",
          "<start_of_image>", "<end_of_image>",
          "<start_of_audio>", "<end_of_audio>",
      };
      break;
    case ModelFamily::kQwen:
      tokens.strip = {
          "<|im_start|>", "<|im_end|>",
          "<tool_call>", "</tool_call>",
      };
      break;
    case ModelFamily::kUnknown:
      break;
  }
  return tokens;
}

ModelSpecialTokens ResolveSpecialTokens(const TokenizerProbe* probe,
                                        ModelFamily declared_family) {
  if (probe != nullptr) {
    ModelSpecialTokens probed = ProbeModelSpecialTokens(*probe);
    if (!probed.empty()) return probed;
  }
  return FamilySpecialTokens(declared_family);
}

}  // namespace agentflow
