// agentflow/inference/litert_lm_engine.h
#ifndef AGENTFLOW_INFERENCE_LITERT_LM_ENGINE_H_
#define AGENTFLOW_INFERENCE_LITERT_LM_ENGINE_H_

#include <memory>
#include <string>

#include "agentflow/inference/special_token_probe.h"
#include "c/engine.h"

namespace agentflow {

struct LiteRtLmEngineOptions {
  std::string model_path;
  std::string backend = "cpu";
  std::string cache_dir;
  int max_num_tokens = 4096;
  // Model family the host wired. Selects the special-token set stripped from
  // unconstrained output (see special_token_probe.h). kUnknown (default)
  // disables decoding until the tokenizer probe path is available.
  ModelFamily model_family = ModelFamily::kUnknown;
};

// Shared wrapper around LiteRtLmEngine*. One engine per model file.
// Thread-safe: multiple sessions can be created concurrently.
class LiteRtLmEngine {
 public:
  static std::shared_ptr<LiteRtLmEngine> Create(LiteRtLmEngineOptions opts);
  ~LiteRtLmEngine();

  LiteRtLmEngine(const LiteRtLmEngine&) = delete;
  LiteRtLmEngine& operator=(const LiteRtLmEngine&) = delete;

  // Returns the raw engine pointer (for creating sessions).
  ::LiteRtLmEngine* Get() const { return engine_; }

  // The model's registered special tokens (tokenizer-verified), in
  // detokenized form. Probed once at creation; empty for unknown families.
  const ModelSpecialTokens& special_tokens() const { return special_tokens_; }

 private:
  LiteRtLmEngine(::LiteRtLmEngine* engine) : engine_(engine) {}
  ::LiteRtLmEngine* engine_;
  ModelSpecialTokens special_tokens_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_INFERENCE_LITERT_LM_ENGINE_H_
