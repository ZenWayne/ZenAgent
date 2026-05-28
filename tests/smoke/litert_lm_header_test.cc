// tests/smoke/litert_lm_header_test.cc
// Verifies the LiteRT-LM C API header compiles.
#include "c/engine.h"

int main() {
    // Just verify the header is parseable and types exist.
    LiteRtLmEngine* e = nullptr;
    LiteRtLmSession* s = nullptr;
    LiteRtLmSamplerParams params;
    params.type = kGreedy;
    params.top_k = 1;
    params.temperature = 0.0f;
    (void)e;
    (void)s;
    (void)params;
    return 0;
}
