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

}  // namespace
}  // namespace agentflow::mcp
