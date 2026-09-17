// tests/unit/net/https_client_integration_test.cc
//
// Opt-in. Controlled by four env vars:
//   AGENTFLOW_TEST_HTTP_URL   - an OpenAI-compatible /v1/chat/completions
//                               endpoint, e.g. http://127.0.0.1:11434/v1/chat/
//                               completions for a local Ollama, or a
//                               https:// URL to exercise the TLS path.
//   AGENTFLOW_TEST_HTTP_MODEL - the model name to put in the request body,
//                               e.g. "gemma4:e2b" for Ollama.
//   AGENTFLOW_TEST_HTTP_KEY   - optional. If set, sent as
//                               "Authorization: Bearer <key>" (a local Ollama
//                               needs no key; a cloud provider does).
//   AGENTFLOW_TEST_CA_PATH    - optional. CA bundle file or hashed CA
//                               directory used to verify an https:// URL.
//                               Falls back to the desktop bundle
//                               (/etc/ssl/certs/ca-certificates.crt) when
//                               unset, so a test proxy with a self-signed
//                               cert (e.g. a local TLS-terminating proxy)
//                               can be verified without hardcoding its path
//                               here — it's a temporary local artifact, not
//                               something this file should know about.
// The HttpsClientIntegrationTest cases skip unless URL and MODEL are set, so
// they are skipped by default. They work against ANY OpenAI-compatible https
// endpoint, publicly-trusted or a private test proxy -- including the TLS
// reject case, which anchors on a CA that signed nothing rather than assuming
// anything about the endpoint's own issuer.
//
// The HttpsClientLocalTest cases need no endpoint at all -- they serve their
// own one-shot HTTP response on an ephemeral port -- so they always run.
#include "agentflow/net/https_client.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"

namespace agentflow::net {
namespace {

using json = nlohmann::json;

struct LiveEndpoint {
  std::string url;
  std::string model;
  std::string key;  // may be empty
};

// Reads AGENTFLOW_TEST_HTTP_URL / _MODEL / _KEY. Returns nullopt (and the
// caller should GTEST_SKIP) when URL or MODEL is unset.
std::optional<LiveEndpoint> GetLiveEndpoint() {
  const char* url = std::getenv("AGENTFLOW_TEST_HTTP_URL");
  const char* model = std::getenv("AGENTFLOW_TEST_HTTP_MODEL");
  if (!url || !model) return std::nullopt;
  LiveEndpoint ep;
  ep.url = url;
  ep.model = model;
  if (const char* key = std::getenv("AGENTFLOW_TEST_HTTP_KEY")) ep.key = key;
  return ep;
}

// CA bundle/directory used to verify an https:// live endpoint. Overridable
// via AGENTFLOW_TEST_CA_PATH so a temporary local test proxy's self-signed
// cert can be verified without this file hardcoding its path; falls back to
// the desktop bundle otherwise.
std::string GetCaPath() {
  if (const char* ca = std::getenv("AGENTFLOW_TEST_CA_PATH")) return ca;
  return "/etc/ssl/certs/ca-certificates.crt";
}

// A trust anchor that is wrong for EVERY endpoint: a self-signed CA that
// signed nothing, so no real chain can verify against it. Embedded rather
// than generated at runtime so the test needs no openssl, and it is a public
// certificate only -- there is no private key here. Valid until 2126.
constexpr char kUnrelatedCaPem[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDLzCCAhegAwIBAgIUN5LbqQldyiDBcZBKPTifXhtUwvIwDQYJKoZIhvcNAQEL
BQAwJjEkMCIGA1UEAwwbYWdlbnRmbG93LXRlc3QtdW5yZWxhdGVkLWNhMCAXDTI2
MDkxNzA0MDAxMVoYDzIxMjYwODI0MDQwMDExWjAmMSQwIgYDVQQDDBthZ2VudGZs
b3ctdGVzdC11bnJlbGF0ZWQtY2EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK
AoIBAQCZR+Zn+HpdQ41EcyER0K5OYX5CfHeUqwlrb5Q4hh9cPvJcNiNjZuECSLTX
cJUx5nSVCMuj4cge05PgSqRCx+TXAIB4jO6UpudwgtMH7bX4QijvSTZCSCq7wMJz
K8IVarF0XVEoNOUxeoEcXK1SwW917ynkDFVEIlodLwDIIJrGVf9N7iGDHytvfah2
9aUOpqRNgq3hHD1ZHO5blEeHxmLDpopuvvW7v3oJcjxR8F2cZxyXpPaHyXeQa0iH
emgb68idtmRiJ+Gmfg1OwpbSB5MWkQDYL2YHw7py7yCdfldGYRW+veV7LMMRSo50
X+n5uUWb/TN2afn8xqjfw64YS/cDAgMBAAGjUzBRMB0GA1UdDgQWBBT+uXuCUCav
R2DbcTuAxmhuuwGXBzAfBgNVHSMEGDAWgBT+uXuCUCavR2DbcTuAxmhuuwGXBzAP
BgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQAzbiMOVDrf1ACkUnmM
pJO4LcQZZcwFNkrqobzak5D8eRZfUxg68rAtWrngXWJq80v/8chYspqgkEmDw8h8
/rMQ+Sr+BNZH/anzyr2LvSaFM+QHfp8ad98jw0VuUxxCDBT0ZSm2Pm7GKKV/E0oc
NbIkKjeNf+y3z2dfQrOuxJXIe44ImXVu65mKdcP3qftX2tf0IwA1AX+J9LGd0Vbv
WsOXtA569JnXezzGpxVGrU4FAoegVPmNosLl1H2mhgg94yX+mUCJd3GVNuDuS33y
a2x4HMSdBO4Vq4ngUu6KfF/jnBZcDMHdJUSZ/NgUqCcb8d8x3qmaHwq9niESgGos
Adr5
-----END CERTIFICATE-----
)PEM";

// Materializes kUnrelatedCaPem on disk (ca_path is a path, not a blob) and
// returns it. TEST_TMPDIR is bazel's per-test scratch dir.
std::string WriteUnrelatedCa() {
  std::filesystem::path dir;
  if (const char* t = std::getenv("TEST_TMPDIR")) {
    dir = t;
  } else {
    dir = std::filesystem::temp_directory_path();
  }
  const std::filesystem::path path = dir / "agentflow_unrelated_ca.pem";
  std::ofstream out(path);
  out << kUnrelatedCaPem;
  out.close();
  return path.string();
}

HttpRequest BuildChatRequest(const LiveEndpoint& ep, bool stream) {
  json body = {
      {"model", ep.model},
      {"messages",
       json::array({{{"role", "user"}, {"content", "Say hi in one word."}}})},
      {"stream", stream},
      {"max_tokens", 8},
  };

  HttpRequest req;
  req.url = ep.url;
  req.body = body.dump();
  req.headers = {{"Content-Type", "application/json"}};
  if (!ep.key.empty()) {
    req.headers.push_back({"Authorization", "Bearer " + ep.key});
  }
  return req;
}

TEST(HttpsClientIntegrationTest, PostReturnsABody) {
  auto ep = GetLiveEndpoint();
  if (!ep) {
    GTEST_SKIP() << "AGENTFLOW_TEST_HTTP_URL / AGENTFLOW_TEST_HTTP_MODEL not "
                    "set";
  }

  asio::io_context io;
  HttpsClientOptions opts;
  opts.ca_path = GetCaPath();
  opts.read_timeout = std::chrono::milliseconds(120'000);
  HttpsClient client(io, opts);

  HttpRequest req = BuildChatRequest(*ep, /*stream=*/false);

  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await client.Post(req, cancel.Token());
      },
      asio::use_future);
  io.run();

  auto body = fut.get();
  ASSERT_TRUE(body.ok()) << body.status().message();
  EXPECT_FALSE(body->empty());

  // A body that is merely non-empty could be an error page — parse it and
  // check for the OpenAI chat-completions shape.
  auto parsed = json::parse(*body, /*cb=*/nullptr, /*allow_exceptions=*/false);
  ASSERT_FALSE(parsed.is_discarded()) << "response body is not valid JSON: "
                                       << *body;
  ASSERT_TRUE(parsed.contains("choices")) << "response body: " << *body;
  EXPECT_TRUE(parsed["choices"].is_array());
}

TEST(HttpsClientIntegrationTest, PostSseDeliversFrames) {
  auto ep = GetLiveEndpoint();
  if (!ep) {
    GTEST_SKIP() << "AGENTFLOW_TEST_HTTP_URL / AGENTFLOW_TEST_HTTP_MODEL not "
                    "set";
  }

  asio::io_context io;
  HttpsClientOptions opts;
  opts.ca_path = GetCaPath();
  opts.read_timeout = std::chrono::milliseconds(120'000);
  HttpsClient client(io, opts);

  HttpRequest req = BuildChatRequest(*ep, /*stream=*/true);

  std::vector<std::string> frames;
  SseHandler on_event = [&](std::string_view data) -> asio::awaitable<void> {
    frames.emplace_back(data);
    co_return;
  };

  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::Status> {
        co_return co_await client.PostSse(req, on_event, cancel.Token());
      },
      asio::use_future);
  io.run();

  auto status = fut.get();
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_FALSE(frames.empty()) << "no SSE frames were delivered";

  for (const auto& frame : frames) {
    auto parsed =
        json::parse(frame, /*cb=*/nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(parsed.is_discarded())
        << "SSE frame is not valid JSON: " << frame;
    ASSERT_TRUE(parsed.contains("choices")) << "SSE frame: " << frame;
    // Not asserting on text content: output is nondeterministic and this
    // model emits reasoning deltas with empty content, so a non-empty-text
    // assertion would flake.
  }
}

TEST(HttpsClientIntegrationTest, RejectsAServerCertItDoesNotTrust) {
  // The accept-path tests above would pass identically against a client that
  // silently accepted ANY certificate chain. This is the case that catches
  // that: the same live endpoint, anchored on a CA that signed nothing.
  //
  // The anchor has to be wrong for EVERY endpoint, not merely some. An earlier
  // version used the system CA store, which is only the "wrong" anchor when the
  // endpoint presents a private certificate (a local TLS-terminating test
  // proxy). Point the suite at a publicly-trusted endpoint instead -- the
  // obvious thing to do, e.g. https://api.deepseek.com/v1/chat/completions --
  // and the system store becomes the RIGHT anchor: the handshake correctly
  // succeeds and the assertion below inverts into a false alarm announcing
  // that verification is not enforced, when it demonstrably is. An unrelated
  // self-signed CA is wrong for the public endpoint and the private proxy
  // alike, so this runs wherever a live https endpoint is configured.
  auto ep = GetLiveEndpoint();
  if (!ep) {
    GTEST_SKIP() << "AGENTFLOW_TEST_HTTP_URL / AGENTFLOW_TEST_HTTP_MODEL not "
                    "set";
  }
  if (ep->url.rfind("https://", 0) != 0) {
    // Running this against the plaintext endpoint would prove nothing (no
    // TLS handshake happens at all) and would fail for the wrong reason.
    GTEST_SKIP() << "AGENTFLOW_TEST_HTTP_URL is not https://; this test only "
                    "exercises the TLS reject path";
  }

  asio::io_context io;
  HttpsClientOptions opts;
  opts.ca_path = WriteUnrelatedCa();  // deliberately the WRONG trust anchor
  opts.read_timeout = std::chrono::milliseconds(120'000);
  HttpsClient client(io, opts);

  HttpRequest req = BuildChatRequest(*ep, /*stream=*/false);

  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await client.Post(req, cancel.Token());
      },
      asio::use_future);
  io.run();

  auto result = fut.get();
  // Not asserting a specific status code or message: TLS failures surface
  // differently across OpenSSL/BoringSSL versions, which would make a tighter
  // assertion brittle. The property that matters is that the call does NOT
  // succeed. That the endpoint itself is reachable is established by the
  // accept-path tests in the same run.
  ASSERT_FALSE(result.ok())
      << "handshake succeeded against a chain anchored on a CA that signed "
         "nothing — certificate verification is not actually being enforced";
  // ...but it must have failed AT VERIFICATION, not because the anchor file
  // was unreadable. That would fail the call for the wrong reason and let a
  // client with verification disabled sail through this test.
  EXPECT_EQ(std::string(result.status().message()).find("cannot load ca_path"),
            std::string::npos)
      << "failed before verification even ran: " << result.status().message();
}

TEST(HttpsClientIntegrationTest, RejectsUnsupportedScheme) {
  // Runs everywhere: URL validation needs no network.
  asio::io_context io;
  HttpsClient client(io, HttpsClientOptions{});

  HttpRequest req;
  req.url = "ftp://example.com/x";

  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::StatusOr<std::string>> {
        co_return co_await client.Post(req, cancel.Token());
      },
      asio::use_future);
  io.run();

  auto r = fut.get();
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), absl::StatusCode::kInvalidArgument);
}

// --- Local server, no live endpoint needed ---------------------------------
//
// Everything above is opt-in and skips by default, so the read loop's own
// end-of-stream handling had no coverage that actually runs. These do: a
// one-shot in-process HTTP server on an ephemeral port, over plain http://
// (the client drives the same object for both schemes and skips the handshake
// for http, so no TLS setup is needed).

// Serves `response` verbatim to exactly one connection, then closes. Returns
// the port it is listening on; the coroutine is already accepting by then.
uint16_t ServeOnce(asio::io_context& io, std::string response) {
  auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(
      io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  const uint16_t port = acceptor->local_endpoint().port();
  asio::co_spawn(io,
      [acceptor, response = std::move(response)]() -> asio::awaitable<void> {
        asio::ip::tcp::socket sock =
            co_await acceptor->async_accept(asio::use_awaitable);
        asio::streambuf buf;
        // Read just the request head; the body is irrelevant to these tests.
        co_await asio::async_read_until(sock, buf, "\r\n\r\n",
                                        asio::use_awaitable);
        co_await asio::async_write(sock, asio::buffer(response),
                                   asio::use_awaitable);
        // Close WITHOUT a graceful shutdown handshake: this is the "server
        // hung up" path the framer flush exists for.
        asio::error_code ignored;
        sock.close(ignored);
        co_return;
      },
      asio::detached);
  return port;
}

std::vector<std::string> CollectSseFrames(const std::string& response,
                                          absl::Status* out_status) {
  asio::io_context io;
  const uint16_t port = ServeOnce(io, response);

  HttpsClientOptions opts;
  HttpsClient client(io, opts);
  HttpRequest req;
  req.url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  req.headers = {{"Content-Type", "application/json"}};
  req.body = "{}";

  std::vector<std::string> frames;
  SseHandler on_event = [&](std::string_view data) -> asio::awaitable<void> {
    frames.emplace_back(data);
    co_return;
  };
  CancelSource cancel;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<absl::Status> {
        co_return co_await client.PostSse(req, on_event, cancel.Token());
      },
      asio::use_future);
  io.run();
  *out_status = fut.get();
  return frames;
}

TEST(HttpsClientLocalTest, DeliversAFinalFrameTheServerLeftUnterminated) {
  // The last frame has no terminating blank line and no [DONE]: the server
  // wrote it and hung up. Before the flush, the read loop simply broke and
  // this frame -- the model's final token -- was dropped on the floor.
  const std::string response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "\r\n"
      "data: {\"a\":1}\n\n"
      "data: {\"b\":2}";

  absl::Status status;
  auto frames = CollectSseFrames(response, &status);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(frames, (std::vector<std::string>{R"({"a":1})", R"({"b":2})"}));
}

TEST(HttpsClientLocalTest, DoesNotInventAFrameAfterACleanDoneSignOff) {
  // Properly terminated and signed off: nothing extra may be delivered, and
  // trailing bytes after [DONE] are noise, not an event.
  const std::string response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "\r\n"
      "data: {\"a\":1}\n\n"
      "data: [DONE]\n\n"
      ": trailing keep-alive";

  absl::Status status;
  auto frames = CollectSseFrames(response, &status);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(frames, (std::vector<std::string>{R"({"a":1})"}));
}

}  // namespace
}  // namespace agentflow::net
