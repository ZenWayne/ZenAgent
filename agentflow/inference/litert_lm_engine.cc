// agentflow/inference/litert_lm_engine.cc
#include "agentflow/inference/litert_lm_engine.h"

namespace agentflow {

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
  return std::shared_ptr<LiteRtLmEngine>(new LiteRtLmEngine(engine));
}

LiteRtLmEngine::~LiteRtLmEngine() {
  if (engine_) litert_lm_engine_delete(engine_);
}

}  // namespace agentflow
