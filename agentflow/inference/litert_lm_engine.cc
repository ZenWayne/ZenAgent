// agentflow/inference/litert_lm_engine.cc
#include "agentflow/inference/litert_lm_engine.h"

#include <optional>
#include <string>
#include <vector>

namespace agentflow {

// The engine's C tokenizer API (litert_lm_engine_tokenize/detokenize) exists
// in the 90f42140-era archive exports but is stripped from the 26df558b line's
// split archives. Define this when the upgrade lands and the archive C API
// regains it — the probe then auto-infers the family and verifies each
// candidate against the tokenizer (paths still work unmodified otherwise).
#ifndef AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API
#define AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API 0
#endif

namespace {
#if AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API

// C-API shim for ProbeModelSpecialTokens. Nullopt on any engine-side error
// (e.g. the tokenizer is unavailable), which degrades to the declared-family
// registry.
TokenizerProbe MakeTokenizerProbe(::LiteRtLmEngine* engine) {
  TokenizerProbe probe;
  probe.tokenize = [engine](std::string_view text)
      -> std::optional<std::vector<int>> {
    auto* result = litert_lm_engine_tokenize(engine, std::string(text).c_str());
    if (!result) return std::nullopt;
    const size_t n = litert_lm_tokenize_result_get_num_tokens(result);
    const int* ids = litert_lm_tokenize_result_get_tokens(result);
    std::vector<int> out(ids, ids + n);
    litert_lm_tokenize_result_delete(result);
    return out;
  };
  probe.detokenize = [engine](const std::vector<int>& ids)
      -> std::optional<std::string> {
    auto* result =
        litert_lm_engine_detokenize(engine, ids.data(), ids.size());
    if (!result) return std::nullopt;
    const char* text = litert_lm_detokenize_result_get_string(result);
    std::string out = text ? text : "";
    litert_lm_detokenize_result_delete(result);
    return out;
  };
  return probe;
}

#endif  // AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API
}  // namespace

std::shared_ptr<LiteRtLmEngine> LiteRtLmEngine::Create(
    LiteRtLmEngineOptions opts) {
  auto* settings = litert_lm_engine_settings_create(
      opts.model_path.c_str(), opts.backend.c_str(),
      /*vision_backend=*/nullptr, /*audio_backend=*/nullptr);
  if (opts.max_num_tokens > 0) {
    litert_lm_engine_settings_set_max_num_tokens(settings, opts.max_num_tokens);
  }
  if (!opts.cache_dir.empty()) {
    litert_lm_engine_settings_set_cache_dir(settings, opts.cache_dir.c_str());
  }
  auto* engine = litert_lm_engine_create(settings);
  litert_lm_engine_settings_delete(settings);
  if (!engine) return nullptr;
  auto wrapper =
      std::shared_ptr<LiteRtLmEngine>(new LiteRtLmEngine(engine));
#if AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API
  TokenizerProbe probe = MakeTokenizerProbe(engine);
  wrapper->special_tokens_ = ResolveSpecialTokens(&probe, opts.model_family);
#else
  wrapper->special_tokens_ = ResolveSpecialTokens(nullptr, opts.model_family);
#endif
  return wrapper;
}

LiteRtLmEngine::~LiteRtLmEngine() {
  if (engine_) litert_lm_engine_delete(engine_);
}

}  // namespace agentflow
