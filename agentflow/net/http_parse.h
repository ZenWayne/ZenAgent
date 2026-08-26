// agentflow/net/http_parse.h
//
// Pure HTTP/1.1 and SSE parsing. No sockets, no TLS, no io_context — every
// entity here is directly unit-testable, which is where the bulk of the
// client's correctness risk lives.
#ifndef AGENTFLOW_NET_HTTP_PARSE_H_
#define AGENTFLOW_NET_HTTP_PARSE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"

namespace agentflow::net {

struct ParsedUrl {
  std::string host;
  std::string port;    // always populated: "443" / "80" / explicit
  std::string target;  // path + query; "/" when the URL has none
  bool tls = false;
};

// Accepts http:// and https:// only. Any other scheme, or a missing scheme,
// is an InvalidArgumentError.
absl::StatusOr<ParsedUrl> ParseUrl(std::string_view url);

struct ResponseHead {
  int status_code = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercased
  // Bytes consumed by the status line + headers + terminator. ZERO means the
  // head is not complete yet and the caller must read more.
  std::size_t head_bytes = 0;
  bool chunked = false;
  std::int64_t content_length = -1;  // -1 when absent
};

// Parses as much of a response head as `buf` contains. An incomplete head is
// NOT an error — it returns head_bytes == 0. A malformed status line is.
absl::StatusOr<ResponseHead> ParseResponseHead(std::string_view buf);

// Incremental HTTP/1.1 chunked-transfer decoder. Feed raw body bytes as they
// arrive; each call returns whatever decoded payload became available.
class ChunkedDecoder {
 public:
  absl::StatusOr<std::string> Feed(std::string_view bytes);
  // True once the terminating zero-length chunk has been seen.
  bool complete() const { return complete_; }

 private:
  std::string buf_;         // undecoded remainder
  std::size_t remaining_ = 0;  // bytes left in the current chunk
  bool in_chunk_ = false;
  bool complete_ = false;
};

// Incremental Server-Sent Events framer. Feed raw (already de-chunked) bytes;
// each call returns the `data:` payloads of every frame that completed.
//
// Comments (": ..."), `event:` lines and other field names are ignored — the
// OpenAI stream only uses `data:`. The literal payload "[DONE]" is consumed
// and reported through saw_done() rather than delivered.
class SseFramer {
 public:
  std::vector<std::string> Feed(std::string_view bytes);
  bool saw_done() const { return saw_done_; }

 private:
  std::string buf_;
  bool saw_done_ = false;
};

}  // namespace agentflow::net
#endif  // AGENTFLOW_NET_HTTP_PARSE_H_
