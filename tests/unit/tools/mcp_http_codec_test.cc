#include "agentflow/tools/mcp_http_codec.h"

#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace agentflow::mcp {
namespace {

using Headers = std::vector<std::pair<std::string, std::string>>;

TEST(McpHttpCodec, HeadersOmitSessionWhenEmpty) {
  auto h = BuildMcpHttpHeaders("");
  bool has_session = false;
  bool has_accept = false;
  for (const auto& [k, v] : h) {
    if (k == "Mcp-Session-Id") has_session = true;
    if (k == "Accept") {
      has_accept = true;
      EXPECT_EQ(v, "application/json, text/event-stream");
    }
  }
  EXPECT_FALSE(has_session);
  EXPECT_TRUE(has_accept);
}

TEST(McpHttpCodec, HeadersCarrySessionWhenPresent) {
  auto h = BuildMcpHttpHeaders("abc123");
  bool found = false;
  for (const auto& [k, v] : h) {
    if (k == "Mcp-Session-Id") {
      found = true;
      EXPECT_EQ(v, "abc123");
    }
  }
  EXPECT_TRUE(found);
}

TEST(McpHttpCodec, SessionIdIsCaseInsensitive) {
  Headers h = {{"MCP-Session-Id", "XYZ"}};
  EXPECT_EQ(SessionIdFromHeaders(h), "XYZ");
  Headers lower = {{"mcp-session-id", "xyz"}};
  EXPECT_EQ(SessionIdFromHeaders(lower), "xyz");
  Headers none = {{"content-type", "application/json"}};
  EXPECT_EQ(SessionIdFromHeaders(none), "");
}

TEST(McpHttpCodec, DecodesSseBody) {
  Headers h = {{"content-type", "text/event-stream"}};
  const std::string body =
      "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":1,"
      "\"result\":{\"ok\":true}}\n\n";
  auto got = DecodeMcpHttpResponse(200, h, body);
  ASSERT_TRUE(got.ok()) << got.status().message();
  ASSERT_TRUE(got->has_value());
  EXPECT_EQ((**got)["id"], 1);
  EXPECT_EQ((**got)["result"]["ok"], true);
}

TEST(McpHttpCodec, DecodesPlainJsonBody) {
  Headers h = {{"content-type", "application/json"}};
  auto got = DecodeMcpHttpResponse(
      200, h, "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":42}");
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(got->has_value());
  EXPECT_EQ((**got)["id"], 7);
}

TEST(McpHttpCodec, NotificationAckIsEmptyPayload) {
  Headers h = {{"content-type", "application/json"}};
  auto got = DecodeMcpHttpResponse(202, h, "");
  ASSERT_TRUE(got.ok());
  EXPECT_FALSE(got->has_value());
}

TEST(McpHttpCodec, LastFrameWinsWhenServerSendsSeveral) {
  Headers h = {{"content-type", "text/event-stream"}};
  const std::string body =
      "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"log\"}\n\n"
      "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":2,\"result\":9}\n\n";
  auto got = DecodeMcpHttpResponse(200, h, body);
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(got->has_value());
  EXPECT_EQ((**got)["id"], 2);
}

TEST(McpHttpCodec, NonSuccessStatusCarriesBodyInMessage) {
  Headers h = {{"content-type", "text/plain"}};
  auto got = DecodeMcpHttpResponse(404, h, "no such session");
  ASSERT_FALSE(got.ok());
  EXPECT_NE(got.status().message().find("no such session"), std::string::npos);
  EXPECT_NE(got.status().message().find("404"), std::string::npos);
}

TEST(McpHttpCodec, MalformedJsonIsInvalidArgument) {
  Headers h = {{"content-type", "application/json"}};
  auto got = DecodeMcpHttpResponse(200, h, "{not json");
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpHttpCodec, SuccessStatusWithNoFramesIsInvalidArgument) {
  Headers h = {{"content-type", "text/event-stream"}};
  auto got = DecodeMcpHttpResponse(200, h, "event: ping\n\n");
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace agentflow::mcp
