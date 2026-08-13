// agentflow/net/https_client.cc
#include "agentflow/net/https_client.h"

#include <sys/stat.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <asio/as_tuple.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "agentflow/net/http_parse.h"

// VARIANT A only:
#include <asio/ssl.hpp>
#include <asio/ssl/host_name_verification.hpp>

namespace agentflow::net {
namespace {

// Builds the request bytes. Connection: close because there is no pooling —
// the server closing the socket is our end-of-body signal for non-chunked
// responses.
std::string BuildRequestBytes(const ParsedUrl& url, const HttpRequest& req,
                               bool sse) {
  std::string out = absl::StrCat("POST ", url.target, " HTTP/1.1\r\n",
                                  "Host: ", url.host, "\r\n",
                                  "Connection: close\r\n",
                                  "Content-Length: ", req.body.size(), "\r\n");
  if (sse) absl::StrAppend(&out, "Accept: text/event-stream\r\n");
  for (const auto& [k, v] : req.headers) {
    absl::StrAppend(&out, k, ": ", v, "\r\n");
  }
  absl::StrAppend(&out, "\r\n", req.body);
  return out;
}

bool IsDirectory(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// The absl codes Task 12's retry policy keys on. Design spec §6.
absl::StatusCode MapHttpStatus(int code) {
  if (code == 401 || code == 403) return absl::StatusCode::kPermissionDenied;
  if (code == 429) return absl::StatusCode::kResourceExhausted;
  if (code >= 500) return absl::StatusCode::kUnavailable;
  if (code >= 400) return absl::StatusCode::kInvalidArgument;
  return absl::StatusCode::kUnknown;
}

// One request's connection. Held by shared_ptr so the cancellation hook stays
// valid for as long as the socket does.
//
// A single ssl::stream is used for both schemes: for https the TLS layer is
// driven directly, for plain http the same object's next_layer() (the bare
// tcp::socket) is used and no handshake happens.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(asio::io_context& io, HttpsClientOptions opts)
      : opts_(std::move(opts)),
        ssl_ctx_(asio::ssl::context::tls_client),
        stream_(io, ssl_ctx_),
        deadline_(io) {}

  // Idempotent and safe from any thread. Pending async ops then complete with
  // operation_aborted, which is how both cancellation and timeout unblock a
  // stalled co_await.
  void Close() {
    asio::error_code ignored;
    stream_.next_layer().close(ignored);
  }

  asio::awaitable<absl::Status> Connect(const ParsedUrl& url) {
    tls_ = url.tls;

    if (tls_) {
      if (opts_.ca_path.empty()) {
        co_return absl::InvalidArgumentError(
            "ca_path is required for https:// URLs; verification is mandatory");
      }
      asio::error_code ec;
      // Set the mode on the STREAM, not the context. stream_ was built in
      // the member-init list, so its SSL* already copied the context's
      // verify mode (SSL_VERIFY_NONE) at construction; a context-level call
      // never reaches it, and set_verify_callback() below re-applies the SSL
      // object's current mode, cementing NONE. Setting it here is what
      // actually enforces validation.
      stream_.set_verify_mode(asio::ssl::verify_peer, ec);
      if (ec) {
        co_return absl::InternalError(
            absl::StrCat("set_verify_mode failed: ", ec.message()));
      }
      // A bundle FILE on desktop, a hashed CA DIRECTORY on Android.
      if (IsDirectory(opts_.ca_path)) {
        ssl_ctx_.add_verify_path(opts_.ca_path, ec);
      } else {
        ssl_ctx_.load_verify_file(opts_.ca_path, ec);
      }
      if (ec) {
        co_return absl::InvalidArgumentError(absl::StrCat(
            "cannot load ca_path '", opts_.ca_path, "': ", ec.message()));
      }
      stream_.set_verify_callback(asio::ssl::host_name_verification(url.host),
                                   ec);
      if (ec) {
        co_return absl::InternalError(
            absl::StrCat("set_verify_callback failed: ", ec.message()));
      }
      // SNI is mandatory at every cloud endpoint and must be set BEFORE the
      // handshake. Unlike the asio calls above this one reports through the
      // raw OpenSSL API, so it is checked separately.
      if (SSL_set_tlsext_host_name(stream_.native_handle(),
                                    url.host.c_str()) != 1) {
        co_return absl::InternalError("failed to set TLS SNI hostname");
      }
    }

    auto executor = co_await asio::this_coro::executor;
    asio::ip::tcp::resolver resolver(executor);

    ArmDeadline(opts_.connect_timeout);
    auto [rec, endpoints] = co_await resolver.async_resolve(
        url.host, url.port, asio::as_tuple(asio::use_awaitable));
    if (rec) {
      DisarmDeadline();
      co_return absl::UnavailableError(
          absl::StrCat("cannot resolve ", url.host, ": ", rec.message()));
    }

    auto [cec, endpoint] = co_await asio::async_connect(
        stream_.next_layer(), endpoints, asio::as_tuple(asio::use_awaitable));
    if (cec) {
      DisarmDeadline();
      co_return absl::UnavailableError(
          absl::StrCat("cannot connect to ", url.host, ":", url.port, ": ",
                        cec.message()));
    }

    if (tls_) {
      auto [hec] = co_await stream_.async_handshake(
          asio::ssl::stream_base::client, asio::as_tuple(asio::use_awaitable));
      if (hec) {
        DisarmDeadline();
        co_return absl::UnavailableError(
            absl::StrCat("TLS handshake with ", url.host, " failed: ",
                          hec.message()));
      }
    }
    DisarmDeadline();
    co_return absl::OkStatus();
  }

  asio::awaitable<absl::Status> WriteAll(const std::string& bytes) {
    ArmDeadline(opts_.read_timeout);
    auto buf = asio::buffer(bytes);
    auto [ec, n] =
        tls_ ? co_await asio::async_write(stream_, buf,
                                           asio::as_tuple(asio::use_awaitable))
             : co_await asio::async_write(stream_.next_layer(), buf,
                                           asio::as_tuple(asio::use_awaitable));
    DisarmDeadline();
    (void)n;
    if (ec) {
      co_return absl::UnavailableError(
          absl::StrCat("request write failed: ", ec.message()));
    }
    co_return absl::OkStatus();
  }

  // Reads whatever is available. Returns an empty string with ok() when the
  // peer closed cleanly — that is end-of-body, not an error, because we send
  // Connection: close.
  asio::awaitable<absl::StatusOr<std::string>> ReadSome() {
    std::array<char, 8192> buf{};
    ArmDeadline(opts_.read_timeout);
    auto asio_buf = asio::buffer(buf);
    auto [ec, n] =
        tls_ ? co_await stream_.async_read_some(
                   asio_buf, asio::as_tuple(asio::use_awaitable))
             : co_await stream_.next_layer().async_read_some(
                   asio_buf, asio::as_tuple(asio::use_awaitable));
    DisarmDeadline();

    if (!ec) co_return std::string(buf.data(), n);
    // Both are ordinary end-of-stream for a Connection: close response.
    if (ec == asio::error::eof ||
        ec == asio::ssl::error::stream_truncated) {
      co_return std::string{};
    }
    co_return absl::UnavailableError(
        absl::StrCat("response read failed: ", ec.message()));
  }

 private:
  // Watchdog: on expiry the socket is closed, so the in-flight async op
  // completes with operation_aborted instead of hanging forever. read_timeout
  // is an IDLE timeout — it is re-armed per read, so a model that streams
  // steadily for minutes is never killed mid-answer.
  void ArmDeadline(std::chrono::milliseconds d) {
    deadline_.expires_after(d);
    auto self = weak_from_this();
    deadline_.async_wait([self](const asio::error_code& ec) {
      if (ec) return;  // cancelled, i.e. the operation finished in time
      if (auto c = self.lock()) c->Close();
    });
  }
  void DisarmDeadline() { deadline_.cancel(); }

  HttpsClientOptions opts_;
  asio::ssl::context ssl_ctx_;                       // declared before stream_
  asio::ssl::stream<asio::ip::tcp::socket> stream_;
  asio::steady_timer deadline_;
  bool tls_ = false;
};

}  // namespace

class HttpsClient::Impl {
 public:
  Impl(asio::io_context& io, HttpsClientOptions opts)
      : io_(io), opts_(std::move(opts)) {}

  // One driver for both entry points. Exactly one of `on_event` / `out_body`
  // is non-null.
  asio::awaitable<absl::Status> Run(HttpRequest req, const SseHandler* on_event,
                                     std::string* out_body,
                                     const CancelToken& cancel) {
    auto url = ParseUrl(req.url);
    if (!url.ok()) co_return url.status();

    auto conn = std::make_shared<Connection>(io_, opts_);
    // A cancel closes the socket, so the in-flight co_await returns an error
    // which the checks below convert to Cancelled.
    //
    // Captured as weak_ptr, not shared_ptr: CancelToken has no way to
    // unregister a callback, so a shared_ptr capture would keep this
    // Connection (and its socket) alive for as long as the CancelSource
    // lives, not just for the duration of Run() — a real leak when one
    // CancelToken is threaded across a whole multi-turn loop with many
    // requests. `conn` is kept alive by the local shared_ptr for the whole
    // of Run(), so the weak_ptr is always lockable while a cancel can still
    // matter, and this closure retains nothing once Run() returns.
    cancel.OnCancel([w = std::weak_ptr<Connection>(conn)]() {
      if (auto c = w.lock()) c->Close();
    });

    if (auto s = co_await conn->Connect(*url); !s.ok()) {
      co_return cancel.IsCancelled() ? absl::CancelledError("cancelled") : s;
    }
    if (auto s = co_await conn->WriteAll(
            BuildRequestBytes(*url, req, /*sse=*/on_event != nullptr));
        !s.ok()) {
      co_return cancel.IsCancelled() ? absl::CancelledError("cancelled") : s;
    }

    // Read the response head.
    std::string raw;
    ResponseHead head;
    for (;;) {
      auto parsed = ParseResponseHead(raw);
      if (!parsed.ok()) co_return parsed.status();
      if (parsed->head_bytes != 0) {
        head = *std::move(parsed);
        break;
      }
      auto chunk = co_await conn->ReadSome();
      if (!chunk.ok()) {
        co_return cancel.IsCancelled() ? absl::CancelledError("cancelled")
                                        : chunk.status();
      }
      if (chunk->empty()) {
        co_return absl::UnavailableError(
            "connection closed before a complete response head arrived");
      }
      raw.append(*chunk);
    }

    std::string body_bytes = raw.substr(head.head_bytes);

    // Reject non-2xx before streaming anything. The error body is bounded —
    // 8 KiB covers every provider's error JSON.
    if (head.status_code < 200 || head.status_code >= 300) {
      constexpr size_t kMaxErrorBody = 8192;
      while (body_bytes.size() < kMaxErrorBody) {
        auto chunk = co_await conn->ReadSome();
        if (!chunk.ok() || chunk->empty()) {
          // A read failure here can be a cancellation (the cancel callback
          // closed the socket): report that distinctly rather than letting
          // it fall through to the ordinary HTTP-status error below, which
          // is what the other read loops in this function already do.
          if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");
          break;
        }
        body_bytes.append(*chunk);
      }
      co_return absl::Status(
          MapHttpStatus(head.status_code),
          absl::StrCat("HTTP ", head.status_code, ": ", body_bytes));
    }

    ChunkedDecoder chunked;
    SseFramer framer;

    // Feeds one slab of raw body bytes onward. Returns false to stop reading.
    auto consume = [&](std::string_view slab) -> asio::awaitable<absl::Status> {
      std::string decoded;
      if (head.chunked) {
        auto d = chunked.Feed(slab);
        if (!d.ok()) co_return d.status();
        decoded = *std::move(d);
      } else {
        decoded = std::string(slab);
      }
      if (out_body) {
        out_body->append(decoded);
        co_return absl::OkStatus();
      }
      for (const auto& payload : framer.Feed(decoded)) {
        // co_await: a slow consumer back-pressures the socket read rather
        // than having frames dropped.
        co_await (*on_event)(payload);
      }
      co_return absl::OkStatus();
    };

    if (auto s = co_await consume(body_bytes); !s.ok()) co_return s;

    for (;;) {
      if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");
      if (on_event && framer.saw_done()) break;
      if (out_body && head.content_length >= 0 &&
          out_body->size() >= static_cast<size_t>(head.content_length)) {
        break;
      }
      if (head.chunked && chunked.complete()) break;

      auto chunk = co_await conn->ReadSome();
      if (!chunk.ok()) {
        co_return cancel.IsCancelled() ? absl::CancelledError("cancelled")
                                        : chunk.status();
      }
      if (chunk->empty()) break;  // peer closed: end of body
      if (auto s = co_await consume(*chunk); !s.ok()) co_return s;
    }

    conn->Close();
    co_return absl::OkStatus();
  }

 private:
  asio::io_context& io_;
  HttpsClientOptions opts_;
};

HttpsClient::HttpsClient(asio::io_context& io, HttpsClientOptions opts)
    : impl_(std::make_unique<Impl>(io, std::move(opts))) {}

HttpsClient::~HttpsClient() = default;

asio::awaitable<absl::Status> HttpsClient::PostSse(HttpRequest req,
                                                    const SseHandler& on_event,
                                                    const CancelToken& cancel) {
  co_return co_await impl_->Run(std::move(req), &on_event, nullptr, cancel);
}

asio::awaitable<absl::StatusOr<std::string>> HttpsClient::Post(
    HttpRequest req, const CancelToken& cancel) {
  std::string body;
  auto status = co_await impl_->Run(std::move(req), nullptr, &body, cancel);
  if (!status.ok()) co_return status;
  co_return body;
}

}  // namespace agentflow::net
