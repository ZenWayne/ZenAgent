#ifndef AGENTFLOW_WORKFLOW_HMAC_SHA256_H_
#define AGENTFLOW_WORKFLOW_HMAC_SHA256_H_

#include <string>
#include <string_view>

namespace agentflow::workflow {

// Returns base64-encoded HMAC-SHA256(data, key).
std::string HmacSha256Base64(std::string_view key, std::string_view data);

// Returns raw 32-byte SHA-256 digest. Exposed for tests/debugging.
std::string Sha256Raw(std::string_view data);

}  // namespace agentflow::workflow

#endif
