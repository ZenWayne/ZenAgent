// agentflow/net/http_client.h
//
// The HTTP surface the OpenAI backend consumes. Kept separate from the
// implementation so tests can inject a fake and exercise request building and
// response mapping with no network.
#ifndef AGENTFLOW_NET_HTTP_CLIENT_H_
#define AGENTFLOW_NET_HTTP_CLIENT_H_

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"

namespace agentflow::net {

struct HttpRequest {
  std::string url;  // http(s)://host[:port]/path
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

// What the response line + headers carried. Header names are lowercased.
struct HttpResponseHead {
  int status_code = 0;
  std::vector<std::pair<std::string, std::string>> headers;
};

// One SSE frame's data payload, with "data: " already stripped. The [DONE]
// sentinel is never delivered — it terminates the stream instead.
//
// Returns an awaitable and is always co_awaited by the client's read loop, so
// a slow consumer back-pressures the socket read rather than losing frames.
using SseHandler =
    std::function<asio::awaitable<void>(std::string_view data)>;

class IHttpClient {
 public:
  virtual ~IHttpClient() = default;

  // POSTs and streams the response, invoking `on_event` per SSE frame.
  // Returns OK once the stream ends cleanly. A non-2xx status is reported as
  // a non-OK Status whose message contains the response body (callers must
  // scrub credentials before logging — the body itself never carries them).
  virtual asio::awaitable<absl::Status> PostSse(
      HttpRequest req, const SseHandler& on_event,
      const CancelToken& cancel) = 0;

  // POSTs and returns the whole response body. When `out_head` is non-null it
  // receives the status code and response headers — MCP needs the assigned
  // Mcp-Session-Id, which lives only in the headers.
  virtual asio::awaitable<absl::StatusOr<std::string>> Post(
      HttpRequest req, const CancelToken& cancel,
      HttpResponseHead* out_head = nullptr) = 0;
};

}  // namespace agentflow::net
#endif  // AGENTFLOW_NET_HTTP_CLIENT_H_
