// agentflow/net/https_client.h
#ifndef AGENTFLOW_NET_HTTPS_CLIENT_H_
#define AGENTFLOW_NET_HTTPS_CLIENT_H_

#include <chrono>
#include <memory>
#include <string>

#include <asio/io_context.hpp>

#include "agentflow/net/http_client.h"

namespace agentflow::net {

struct HttpsClientOptions {
  // Either a CA bundle FILE (desktop, e.g.
  // /etc/ssl/certs/ca-certificates.crt) or a hashed CA DIRECTORY (Android,
  // /system/etc/security/cacerts/). Both forms are accepted; the client
  // stats the path to decide. Required for https:// URLs — there is
  // deliberately no option to skip verification.
  std::string ca_path;

  std::chrono::milliseconds connect_timeout{10'000};
  // Idle timeout BETWEEN reads, not a whole-response deadline: a slow model
  // that streams steadily must not be killed mid-answer.
  std::chrono::milliseconds read_timeout{60'000};
};

// Minimal HTTP/1.1 client. POST only. Supports chunked and SSE bodies.
//
// Deliberately NOT supported (design spec §4): redirect following, connection
// pooling (one connection per request), HTTP/2.
class HttpsClient : public IHttpClient {
 public:
  HttpsClient(asio::io_context& io, HttpsClientOptions opts);
  ~HttpsClient() override;

  asio::awaitable<absl::Status> PostSse(HttpRequest req,
                                         const SseHandler& on_event,
                                         const CancelToken& cancel) override;

  asio::awaitable<absl::StatusOr<std::string>> Post(
      HttpRequest req, const CancelToken& cancel) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace agentflow::net
#endif  // AGENTFLOW_NET_HTTPS_CLIENT_H_
