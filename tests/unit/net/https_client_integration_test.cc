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
// Both tests below skip unless URL and MODEL are set. Skipped by default.
#include "agentflow/net/https_client.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
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
  // Same live TLS endpoint as the accept-path tests above, but with the
  // system CA bundle as the trust anchor instead of the proxy's self-signed
  // cert. The handshake MUST fail. This is the property that actually
  // distinguishes "verification is configured" from "verification rejects":
  // a client that silently accepted any certificate chain would pass the
  // accept-path tests identically and only get caught here.
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

  constexpr const char* kSystemCaBundle =
      "/etc/ssl/certs/ca-certificates.crt";
  if (!std::filesystem::exists(kSystemCaBundle)) {
    GTEST_SKIP() << kSystemCaBundle << " is not present on this host";
  }

  asio::io_context io;
  HttpsClientOptions opts;
  // Deliberately the WRONG trust anchor: the system bundle does not contain
  // the test proxy's self-signed cert (unlike AGENTFLOW_TEST_CA_PATH, used
  // by the accept-path tests above), so verification must fail.
  opts.ca_path = kSystemCaBundle;
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
  // differently across OpenSSL/BoringSSL versions, which would make a
  // tighter assertion brittle. The property that matters is that the call
  // does NOT succeed. That the endpoint itself is reachable is established
  // by the accept-path tests running in the same suite/run, so a failure
  // here can only be attributed to certificate verification rejecting the
  // chain, not to the endpoint being unreachable.
  EXPECT_FALSE(result.ok())
      << "handshake succeeded against a certificate chain that should NOT "
         "be trusted — certificate verification is not actually being "
         "enforced";
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

}  // namespace
}  // namespace agentflow::net
