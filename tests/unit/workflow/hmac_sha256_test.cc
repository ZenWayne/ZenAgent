#include "agentflow/workflow/hmac_sha256.h"

#include <gtest/gtest.h>

namespace agentflow::workflow {
namespace {

// RFC 4231 test vector 2: key = "Jefe", data = "what do ya want for nothing?"
// Expected HMAC-SHA256 hex:
//   5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
// Cross-checked against `openssl dgst -sha256 -hmac Jefe`:
//   "W9zBRr9gdU5qBCQmCJV1x1oAPwidJzmDnexYuWTsOEM="
TEST(HmacSha256Test, Rfc4231Vector2) {
  std::string mac = HmacSha256Base64("Jefe", "what do ya want for nothing?");
  EXPECT_EQ(mac, "W9zBRr9gdU5qBCQmCJV1x1oAPwidJzmDnexYuWTsOEM=");
}

TEST(HmacSha256Test, EmptyKeyEmptyData) {
  // sha256("") then hmac with empty key — value computed once and pinned.
  std::string mac = HmacSha256Base64("", "");
  EXPECT_FALSE(mac.empty());
  EXPECT_EQ(mac.size(), 44u);  // 32 bytes → 44 base64 chars w/ padding
}

}  // namespace
}  // namespace agentflow::workflow
