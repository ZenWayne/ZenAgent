// tests/smoke/litert_lm_header_test.cc
// Verifies the LiteRT-LM C API headers compile against the real engine.h /
// conversation.h (90f42140 line) AND that the prebuilt archives export the
// symbols the framework needs — notably the tokenizer C API, which the
// 26df558b split archives stripped (47 litert_lm_* exports, no tokenize).
#include "c/conversation.h"
#include "c/engine.h"

int main() {
  // Opaque sampler-params type: only the pointer API is valid, and the
  // create/delete pair exercises a real constructor without an engine.
  LiteRtLmSamplerParams* params =
      litert_lm_sampler_params_create(kLiteRtLmSamplerTypeGreedy);
  if (params != nullptr) {
    litert_lm_sampler_params_set_top_k(params, 1);
    litert_lm_sampler_params_set_temperature(params, 0.0f);
    litert_lm_sampler_params_delete(params);
  }

  // Symbol-integrity proof (compile + link only; never called, so the test
  // binary still runs with no engine): these must be exported by the 90f42140
  // archives.
  (void)&litert_lm_engine_tokenize;
  (void)&litert_lm_engine_detokenize;
  (void)&litert_lm_tokenize_result_get_tokens;
  (void)&litert_lm_tokenize_result_get_num_tokens;
  (void)&litert_lm_detokenize_result_get_string;

  // Conversation C bridge (moved to c/conversation.h in 90f42140).
  (void)&litert_lm_conversation_config_create;
  return 0;
}
