// tests/unit/net/https_client_integration_test.cc
//
// Opt-in. Set AGENTFLOW_TEST_HTTP_URL to an endpoint that accepts a POST and
// returns a body — a local Ollama (http://127.0.0.1:11434/api/tags) exercises
// the plain path; any https:// URL exercises TLS. Skipped by default.
#include "agentflow/net/https_client.h"

#include <cstdlib>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"

namespace agentflow::net {
namespace {

TEST(HttpsClientIntegrationTest, PostReturnsABody) {
  const char* url = std::getenv("AGENTFLOW_TEST_HTTP_URL");
  if (!url) GTEST_SKIP() << "AGENTFLOW_TEST_HTTP_URL not set";

  asio::io_context io;
  HttpsClientOptions opts;
  opts.ca_path = "/etc/ssl/certs/ca-certificates.crt";
  HttpsClient client(io, opts);

  HttpRequest req;
  req.url = url;
  req.body = "{}";
  req.headers = {{"Content-Type", "application/json"}};

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
