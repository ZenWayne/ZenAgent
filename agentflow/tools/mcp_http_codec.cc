// agentflow/tools/mcp_http_codec.cc
#include "agentflow/tools/mcp_http_codec.h"

#include <algorithm>
#include <cctype>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "agentflow/net/http_parse.h"

namespace agentflow::mcp {
namespace {

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

std::string HeaderValue(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view name) {
  const std::string want = ToLower(name);
  for (const auto& [k, v] : headers) {
    if (ToLower(k) == want) return v;
  }
  return {};
}

}  // namespace

std::vector<std::pair<std::string, std::string>> BuildMcpHttpHeaders(
    std::string_view session_id) {
  std::vector<std::pair<std::string, std::string>> h = {
      {"Content-Type", "application/json"},
      {"Accept", "application/json, text/event-stream"},
  };
  if (!session_id.empty()) {
    h.emplace_back("Mcp-Session-Id", std::string(session_id));
  }
  return h;
}

std::string SessionIdFromHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers) {
  return HeaderValue(headers, "mcp-session-id");
}

absl::StatusOr<std::optional<nlohmann::json>> DecodeMcpHttpResponse(
    int status_code,
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view body) {
  if (status_code < 200 || status_code >= 300) {
    return absl::InternalError(
        absl::StrCat("MCP HTTP ", status_code, ": ", body));
  }

  // Notification ack: 202 (or any 2xx) with nothing to decode.
  if (body.empty()) return std::optional<nlohmann::json>{};

  const std::string ctype = ToLower(HeaderValue(headers, "content-type"));
  std::string payload;

  if (ctype.find("text/event-stream") != std::string::npos) {
    net::SseFramer framer;
    auto frames = framer.Feed(body);
    if (frames.empty()) {
      return absl::InvalidArgumentError(
          "MCP HTTP: event-stream response contained no data frame");
    }
    payload = frames.back();
  } else {
    payload = std::string(body);
  }

  auto parsed = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return absl::InvalidArgumentError(
        absl::StrCat("MCP HTTP: response body is not valid JSON: ", payload));
  }
  return std::optional<nlohmann::json>{std::move(parsed)};
}

}  // namespace agentflow::mcp
