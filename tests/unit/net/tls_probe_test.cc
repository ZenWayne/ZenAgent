// tests/unit/net/tls_probe_test.cc
//
// Decides how HttpsClient talks TLS (design spec §4.1).
//
// asio::ssl is written against the OpenSSL API. BoringSSL reports
// OPENSSL_VERSION_NUMBER as 1.1.1 so asio takes its 1.1.1 code path, but
// neither project promises compatibility. If this file compiles, links and
// passes, HttpsClient uses asio::ssl. If it does not COMPILE, HttpsClient
// drives BoringSSL manually with memory BIOs instead (Task 8, Variant B).
//
// A compile failure here is a legitimate, expected outcome — not a defect
// to debug. Record which variant applies and move on.

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>

#include <gtest/gtest.h>

TEST(TlsProbe, AsioSslContextConstructsOverBoringSsl) {
  asio::io_context io;
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  ctx.set_verify_mode(asio::ssl::verify_peer);
  asio::ssl::stream<asio::ip::tcp::socket> stream(io, ctx);
  // Reaching here means construction and linkage both work.
  SUCCEED();
}

TEST(TlsProbe, AsioSslLoadsSystemCaBundle) {
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  asio::error_code ec;
  ctx.load_verify_file("/etc/ssl/certs/ca-certificates.crt", ec);
  if (ec) GTEST_SKIP() << "no system CA bundle at the expected path: " << ec.message();
  SUCCEED();
}

TEST(TlsProbe, AsioSslSetsSniHostname) {
  asio::io_context io;
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  asio::ssl::stream<asio::ip::tcp::socket> stream(io, ctx);
  // SNI is mandatory for every cloud endpoint. SSL_set_tlsext_host_name is a
  // macro over SSL_ctrl in both OpenSSL and BoringSSL; verify it resolves.
  ASSERT_EQ(1, SSL_set_tlsext_host_name(stream.native_handle(), "api.openai.com"));
}
