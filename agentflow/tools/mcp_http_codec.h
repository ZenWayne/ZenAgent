// agentflow/tools/mcp_http_codec.h
//
// Pure MCP streamable-HTTP framing. No sockets, no io_context — this is where
// the transport's correctness risk lives, so it is directly unit-testable
// (same split as agentflow/net/http_parse.h).
#ifndef AGENTFLOW_TOOLS_MCP_HTTP_CODEC_H_
#define AGENTFLOW_TOOLS_MCP_HTTP_CODEC_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include <nlohmann/json.hpp>

namespace agentflow::mcp {

// Headers every MCP streamable-HTTP POST carries. `session_id` is empty on the
// initialize call — the server assigns one in its response headers.
std::vector<std::pair<std::string, std::string>> BuildMcpHttpHeaders(
    std::string_view session_id);

// The Mcp-Session-Id a response assigned, or "" when absent. Header names are
// matched case-insensitively: ParseResponseHead lowercases them, but a fake or
// a future caller may not.
std::string SessionIdFromHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers);

// Decodes one MCP streamable-HTTP response into its JSON-RPC payload.
//
// Both response shapes the protocol allows are accepted:
//   - text/event-stream : `data:` frames; the LAST complete frame is the
//     response to this request (earlier frames are server-side logs/progress).
//   - application/json  : the body IS the payload.
//
// A 202 with an empty body is the notification ack — returns nullopt, which is
// success with no payload. Any non-2xx status is an error carrying the body.
absl::StatusOr<std::optional<nlohmann::json>> DecodeMcpHttpResponse(
    int status_code,
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view body);

}  // namespace agentflow::mcp

#endif  // AGENTFLOW_TOOLS_MCP_HTTP_CODEC_H_
