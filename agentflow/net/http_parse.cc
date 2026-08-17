// agentflow/net/http_parse.cc
#include "agentflow/net/http_parse.h"

#include <algorithm>
#include <cctype>
#include <charconv>

#include <absl/status/status.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>

namespace agentflow::net {
namespace {

// Splits `s` at the first occurrence of `sep`. Returns npos-safe halves.
std::pair<std::string_view, std::string_view> SplitOnce(std::string_view s,
                                                         std::string_view sep) {
  const auto pos = s.find(sep);
  if (pos == std::string_view::npos) return {s, {}};
  return {s.substr(0, pos), s.substr(pos + sep.size())};
}

std::string_view TrimAscii(std::string_view s) {
  while (!s.empty() && absl::ascii_isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && absl::ascii_isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

}  // namespace

absl::StatusOr<ParsedUrl> ParseUrl(std::string_view url) {
  ParsedUrl out;
  std::string_view rest;
  if (url.starts_with("https://")) {
    out.tls = true;
    out.port = "443";
    rest = url.substr(8);
  } else if (url.starts_with("http://")) {
    out.tls = false;
    out.port = "80";
    rest = url.substr(7);
  } else {
    return absl::InvalidArgumentError(
        absl::StrCat("unsupported URL scheme: ", url));
  }

  const auto slash = rest.find('/');
  std::string_view authority = rest.substr(0, slash);
  out.target = slash == std::string_view::npos
                    ? std::string("/")
                    : std::string(rest.substr(slash));
  if (authority.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("URL has no host: ", url));
  }

  if (authority.front() == '[') {
    // Bracketed IPv6 literal, e.g. "[::1]" or "[::1]:8443".
    const auto close = authority.find(']');
    if (close == std::string_view::npos) {
      return absl::InvalidArgumentError(
          absl::StrCat("unclosed IPv6 literal in URL: ", url));
    }
    out.host = std::string(authority.substr(1, close - 1));
    if (close + 1 < authority.size() && authority[close + 1] == ':') {
      out.port = std::string(authority.substr(close + 2));
    }
  } else {
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      out.host = std::string(authority.substr(0, colon));
      out.port = std::string(authority.substr(colon + 1));
    } else {
      out.host = std::string(authority);
    }
  }
  if (out.host.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("URL has no host: ", url));
  }
  return out;
}

absl::StatusOr<ResponseHead> ParseResponseHead(std::string_view buf) {
  ResponseHead head;
  const auto term = buf.find("\r\n\r\n");
  if (term == std::string_view::npos) return head;  // incomplete; head_bytes==0

  std::string_view h = buf.substr(0, term);
  auto [status_line, header_block] = SplitOnce(h, "\r\n");

  // "HTTP/1.1 200 OK"
  auto [version, after_version] = SplitOnce(status_line, " ");
  if (!version.starts_with("HTTP/")) {
    return absl::InvalidArgumentError("malformed HTTP status line");
  }
  auto [code_str, _reason] = SplitOnce(after_version, " ");
  int code = 0;
  const auto res = std::from_chars(code_str.data(),
                                    code_str.data() + code_str.size(), code);
  if (res.ec != std::errc{} || code < 100 || code > 599) {
    return absl::InvalidArgumentError("malformed HTTP status code");
  }
  head.status_code = code;

  std::string_view remaining = header_block;
  while (!remaining.empty()) {
    auto [line, tail] = SplitOnce(remaining, "\r\n");
    remaining = tail;
    if (line.empty()) continue;
    auto [name, value] = SplitOnce(line, ":");
    std::string lname = absl::AsciiStrToLower(TrimAscii(name));
    std::string lvalue(TrimAscii(value));
    if (lname == "transfer-encoding" &&
        absl::AsciiStrToLower(lvalue).find("chunked") != std::string::npos) {
      head.chunked = true;
    } else if (lname == "content-length") {
      std::int64_t n = 0;
      if (std::from_chars(lvalue.data(), lvalue.data() + lvalue.size(), n).ec ==
          std::errc{}) {
        head.content_length = n;
      }
    }
    head.headers.emplace_back(std::move(lname), std::move(lvalue));
  }

  head.head_bytes = term + 4;
  return head;
}

absl::StatusOr<std::string> ChunkedDecoder::Feed(std::string_view bytes) {
  buf_.append(bytes);
  std::string out;
  std::size_t pos = 0;

  for (;;) {
    if (complete_) break;
    if (in_chunk_) {
      const std::size_t avail = buf_.size() - pos;
      const std::size_t take = std::min(avail, remaining_);
      out.append(buf_, pos, take);
      pos += take;
      remaining_ -= take;
      if (remaining_ > 0) break;  // need more bytes
      // Consume the CRLF that terminates the chunk body.
      if (buf_.size() - pos < 2) break;
      pos += 2;
      in_chunk_ = false;
      continue;
    }
    // Reading a chunk-size line.
    const auto eol = buf_.find("\r\n", pos);
    if (eol == std::string::npos) break;  // size line incomplete
    std::string_view size_line(buf_.data() + pos, eol - pos);
    // Strip any chunk extension (";name=value").
    size_line = SplitOnce(size_line, ";").first;
    std::size_t n = 0;
    const auto res = std::from_chars(size_line.data(),
                                      size_line.data() + size_line.size(), n, 16);
    if (res.ec != std::errc{}) {
      return absl::InvalidArgumentError("malformed chunk size");
    }
    pos = eol + 2;
    if (n == 0) {
      complete_ = true;
      break;
    }
    remaining_ = n;
    in_chunk_ = true;
  }

  buf_.erase(0, pos);
  return out;
}

std::vector<std::string> SseFramer::Feed(std::string_view bytes) {
  buf_.append(bytes);
  std::vector<std::string> out;

  for (;;) {
    // A frame ends at a blank line. Accept both LF and CRLF forms.
    std::size_t end = buf_.find("\n\n");
    std::size_t sep_len = 2;
    const std::size_t crlf = buf_.find("\r\n\r\n");
    if (crlf != std::string::npos && (end == std::string::npos || crlf < end)) {
      end = crlf;
      sep_len = 4;
    }
    if (end == std::string::npos) break;

    std::string_view frame(buf_.data(), end);
    std::string payload;
    while (!frame.empty()) {
      auto [line, tail] = SplitOnce(frame, "\n");
      frame = tail;
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.empty() || line.front() == ':') continue;  // blank or comment
      auto [name, value] = SplitOnce(line, ":");
      if (name != "data") continue;  // ignore event:, id:, retry:
      // Per the SSE spec a single leading space after the colon is stripped.
      if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
      if (!payload.empty()) payload.push_back('\n');
      payload.append(value);
    }
    buf_.erase(0, end + sep_len);

    if (payload.empty()) continue;
    if (payload == "[DONE]") {
      saw_done_ = true;
      continue;
    }
    out.push_back(std::move(payload));
  }
  return out;
}

}  // namespace agentflow::net
