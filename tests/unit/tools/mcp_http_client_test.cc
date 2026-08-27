#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/net/http_client.h"
#include "agentflow/tools/mcp_client.h"
#include "mcp_spec.pb.h"

namespace agentflow::mcp {
namespace {

using json = nlohmann::json;

// Records every request and replays canned responses in order.
class FakeHttpClient : public net::IHttpClient {
 public:
  struct Canned {
    int status;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };

  std::vector<net::HttpRequest> seen;
  std::vector<Canned> canned;
  std::size_t next = 0;

  asio::awaitable<absl::Status> PostSse(net::HttpRequest, const net::SseHandler&,
                                        const CancelToken&) override {
    co_return absl::UnimplementedError("not used");
  }

  asio::awaitable<absl::StatusOr<std::string>> Post(
      net::HttpRequest req, const CancelToken&,
      net::HttpResponseHead* out_head) override {
    seen.push_back(req);
    if (next >= canned.size()) {
      co_return absl::InternalError("fake: no canned response left");
    }
    const Canned& c = canned[next++];
    if (out_head != nullptr) {
      out_head->status_code = c.status;
      out_head->headers = c.headers;
    }
    // Mirror agentflow/net/https_client.cc: the head is published before the
    // non-2xx bail-out, and a non-2xx status fails the whole Post() call
    // rather than being handed back as an ok body for the codec to reject.
    if (c.status < 200 || c.status >= 300) {
      co_return absl::UnavailableError("fake: HTTP " +
                                        std::to_string(c.status));
    }
    co_return c.body;
  }
};

proto::McpServerSpec HttpSpec() {
  proto::McpServerSpec spec;
  spec.set_transport(proto::McpServerSpec::HTTP_SSE);
  spec.set_command_or_url("http://mcp.test:8765/mcp");
  return spec;
}

std::string SseFrame(const json& payload) {
  return "event: message\ndata: " + payload.dump() + "\n\n";
}

std::string HeaderOf(const net::HttpRequest& req, std::string_view name) {
  for (const auto& [k, v] : req.headers) {
    if (k == name) return v;
  }
  return {};
}

TEST(McpHttpClient, HandshakeThenListTools) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();

  // 1) initialize -> assigns a session id
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S1"}},
       SseFrame({{"jsonrpc", "2.0"},
                 {"id", 1},
                 {"result", {{"protocolVersion", "2024-11-05"}}}})});
  // 2) notifications/initialized -> 202 empty
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  // 3) tools/list
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}},
       SseFrame({{"jsonrpc", "2.0"},
                 {"id", 2},
                 {"result",
                  {{"tools",
                    json::array({{{"name", "get_shot"},
                                  {"description", "read one shot"},
                                  {"inputSchema", {{"type", "object"}}}}})}}}})});

  auto client = McpClient::Create(HttpSpec(), io, fake);

  std::vector<ToolSchema> tools;
  absl::Status status;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto got = co_await client->ListTools();
        status = got.status();
        if (got.ok()) tools = *got;
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].name, "get_shot");

  // initialize must NOT carry a session id; everything after it must.
  ASSERT_EQ(fake->seen.size(), 3u);
  EXPECT_EQ(HeaderOf(fake->seen[0], "Mcp-Session-Id"), "");
  EXPECT_EQ(HeaderOf(fake->seen[1], "Mcp-Session-Id"), "S1");
  EXPECT_EQ(HeaderOf(fake->seen[2], "Mcp-Session-Id"), "S1");
  EXPECT_EQ(json::parse(fake->seen[1].body)["method"],
            "notifications/initialized");
}

TEST(McpHttpClient, CallToolReturnsResultJson) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S2"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}})});
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}},
       SseFrame({{"jsonrpc", "2.0"},
                 {"id", 2},
                 {"result", {{"content", json::array()}, {"isError", false}}}})});

  auto client = McpClient::Create(HttpSpec(), io, fake);

  std::string result;
  absl::Status status;
  CancelToken no_cancel;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto got = co_await client->CallTool("get_shot", R"({"shot_id":5})",
                                             no_cancel);
        status = got.status();
        if (got.ok()) result = *got;
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_NE(result.find("isError"), std::string::npos);

  const auto call = json::parse(fake->seen[2].body);
  EXPECT_EQ(call["method"], "tools/call");
  EXPECT_EQ(call["params"]["name"], "get_shot");
  EXPECT_EQ(call["params"]["arguments"]["shot_id"], 5);
}

TEST(McpHttpClient, ServerErrorSurfacesAsNonOkStatus) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S3"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}})});
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}},
       SseFrame({{"jsonrpc", "2.0"},
                 {"id", 2},
                 {"error", {{"code", -32602}, {"message", "bad params"}}}})});

  auto client = McpClient::Create(HttpSpec(), io, fake);

  absl::Status status;
  CancelToken no_cancel;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto got = co_await client->CallTool("get_shot", "{}", no_cancel);
        status = got.status();
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("bad params"), std::string::npos);
}

TEST(McpHttpClient, StaticSpecHeadersAreSentOnEveryRequest) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S4"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}})});
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}},
       SseFrame({{"jsonrpc", "2.0"},
                 {"id", 2},
                 {"result", {{"tools", json::array()}}}})});

  auto spec = HttpSpec();
  (*spec.mutable_headers())["Authorization"] = "Bearer tok123";
  auto client = McpClient::Create(spec, io, fake);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        (void)co_await client->ListTools();
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_EQ(fake->seen.size(), 3u);
  for (const auto& req : fake->seen) {
    EXPECT_EQ(HeaderOf(req, "Authorization"), "Bearer tok123");
  }
}

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

TEST(McpHttpClient, SpecHeaderOverridesDefaultCaseInsensitively) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S5"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}})});
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}},
       SseFrame({{"jsonrpc", "2.0"},
                 {"id", 2},
                 {"result", {{"tools", json::array()}}}})});

  auto spec = HttpSpec();
  // Differently-cased than BuildMcpHttpHeaders's "Content-Type" default --
  // must override it in place, never append a second Content-Type header.
  (*spec.mutable_headers())["content-type"] = "application/vnd.custom+json";
  auto client = McpClient::Create(spec, io, fake);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        (void)co_await client->ListTools();
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_EQ(fake->seen.size(), 3u);
  for (const auto& req : fake->seen) {
    int count = 0;
    std::string value;
    for (const auto& [k, v] : req.headers) {
      if (LowerAscii(k) == "content-type") {
        ++count;
        value = v;
      }
    }
    EXPECT_EQ(count, 1) << "expected exactly one Content-Type header";
    EXPECT_EQ(value, "application/vnd.custom+json");
  }
}

TEST(McpHttpClient, ResponseIdMismatchIsError) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S6"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}})});
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  // tools/call is request id=2, but the response claims id=99 -- e.g. a
  // trailing progress/notification frame the codec's last-frame heuristic
  // picked up instead of the real response. Must not be accepted as success.
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}},
       SseFrame({{"jsonrpc", "2.0"},
                 {"id", 99},
                 {"result", {{"content", json::array()}, {"isError", false}}}})});

  auto client = McpClient::Create(HttpSpec(), io, fake);

  absl::Status status;
  CancelToken no_cancel;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto got = co_await client->CallTool("get_shot", "{}", no_cancel);
        status = got.status();
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("does not match request id"),
            std::string::npos);
}

TEST(McpHttpClient, ResponseWithNeitherResultNorErrorIsError) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S7"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}})});
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  // Well-formed id, but the payload has neither "result" nor "error" -- a
  // JSON-RPC protocol violation that must not be silently treated as an
  // empty success.
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 2}})});

  auto client = McpClient::Create(HttpSpec(), io, fake);

  absl::Status status;
  CancelToken no_cancel;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto got = co_await client->CallTool("get_shot", "{}", no_cancel);
        status = got.status();
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("neither result nor error"),
            std::string::npos);
}

// F#1: a non-object JSON-RPC payload (e.g. a bare `null` or `[1,2]` SSE data
// frame) must surface as a non-ok status, not throw nlohmann::json::type_error
// out of the coroutine (which would otherwise propagate through
// asio::detached and be rethrown by io_context::run(), very likely killing
// the process).
TEST(McpHttpClient, NonObjectPayloadIsErrorNotThrow) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S8"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}})});
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  // Malformed server: the "response" is valid JSON but not an object.
  fake->canned.push_back(
      {200, {{"content-type", "text/event-stream"}}, SseFrame(json(nullptr))});

  auto client = McpClient::Create(HttpSpec(), io, fake);

  absl::Status status;
  CancelToken no_cancel;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto got = co_await client->CallTool("get_shot", "{}", no_cancel);
        status = got.status();
        co_return;
      },
      asio::detached);
  // io.run() would rethrow an uncaught exception escaping the coroutine --
  // reaching the assertions below at all is part of what this test checks.
  io.run();

  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("not an object"), std::string::npos);
}

// F#2: an empty 2xx body for a real request (not a notification) must not be
// handed to the caller as an empty-but-successful tool result -- that's the
// same swallowed-protocol-violation class the F4 fix closed for the
// last-frame path, arriving through the empty-body door instead.
TEST(McpHttpClient, EmptyBodyForRealRequestIsError) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S9"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}})});
  // notifications/initialized: empty 202 body on the notification path must
  // still succeed (this is not a regression target of the F#2 fix).
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  // tools/call is a real request (is_notification=false) but the server
  // answers 200 with an empty body.
  fake->canned.push_back({200, {{"content-type", "application/json"}}, ""});

  auto client = McpClient::Create(HttpSpec(), io, fake);

  absl::Status status;
  CancelToken no_cancel;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto got = co_await client->CallTool("get_shot", "{}", no_cancel);
        status = got.status();
        co_return;
      },
      asio::detached);
  io.run();

  // The handshake's own notifications/initialized (empty 202) had to succeed
  // for CallTool to even reach the tools/call POST -- so reaching this
  // non-ok assertion already confirms the notification path is unaffected.
  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("empty body"), std::string::npos);
}

// F#3: a failed HTTP POST (transport error / non-2xx, as FakeHttpClient now
// mirrors https_client.cc's non-2xx-fails-Post() behavior) must flip the
// client out of kReady so the NEXT call re-handshakes instead of reusing a
// session the server may have already discarded. There is no public
// state_ accessor, so the state transition is observed indirectly: the next
// ListTools() call must re-send "initialize" (a fresh handshake), visible as
// new entries in fake->seen.
TEST(McpHttpClient, FailedPostBreaksClientAndForcesRehandshake) {
  asio::io_context io;
  auto fake = std::make_shared<FakeHttpClient>();
  // First connect: succeeds and assigns session S10.
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S10"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}})});
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  // First tools/list: the session has expired server-side -- 404.
  fake->canned.push_back({404, {}, "session not found"});
  // Second connect (re-handshake after kBroken): succeeds with a new session.
  // Request ids are a monotonic counter that is NOT reset across reconnects:
  // id=1 was the first initialize, id=2 the first (failed) tools/list, so
  // this second initialize is id=3 and the second tools/list is id=4.
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}, {"mcp-session-id", "S11"}},
       SseFrame({{"jsonrpc", "2.0"}, {"id", 3}, {"result", json::object()}})});
  fake->canned.push_back({202, {{"content-type", "application/json"}}, ""});
  // Second tools/list: succeeds for real.
  fake->canned.push_back(
      {200,
       {{"content-type", "text/event-stream"}},
       SseFrame({{"jsonrpc", "2.0"},
                 {"id", 4},
                 {"result",
                  {{"tools",
                    json::array({{{"name", "get_shot"},
                                  {"description", "read one shot"},
                                  {"inputSchema", {{"type", "object"}}}}})}}}})});

  auto client = McpClient::Create(HttpSpec(), io, fake);

  absl::Status first_status;
  absl::Status second_status;
  std::vector<ToolSchema> tools;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto first = co_await client->ListTools();
        first_status = first.status();

        auto second = co_await client->ListTools();
        second_status = second.status();
        if (second.ok()) tools = *second;
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_FALSE(first_status.ok());

  // If the client had stayed kReady after the broken POST, the second
  // ListTools() would go straight to a 4th canned response (another
  // tools/list) instead of re-running the handshake -- and since only 6
  // responses are canned, that would either consume the wrong canned entry
  // or exhaust `canned` and fail with "no canned response left". Observing
  // a SUCCESSFUL second call, whose request sequence replays a fresh
  // initialize + notifications/initialized + tools/list, is the only way
  // through this canned sequence -- so it demonstrates the client left
  // kReady and re-handshook.
  ASSERT_TRUE(second_status.ok()) << second_status.message();
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].name, "get_shot");

  ASSERT_EQ(fake->seen.size(), 6u);
  EXPECT_EQ(json::parse(fake->seen[3].body)["method"], "initialize");
  EXPECT_EQ(HeaderOf(fake->seen[3], "Mcp-Session-Id"), "")
      << "re-handshake's initialize must not carry the discarded session id";
  EXPECT_EQ(json::parse(fake->seen[4].body)["method"],
            "notifications/initialized");
  EXPECT_EQ(HeaderOf(fake->seen[5], "Mcp-Session-Id"), "S11");
}

}  // namespace
}  // namespace agentflow::mcp
