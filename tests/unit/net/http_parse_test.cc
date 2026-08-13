// tests/unit/net/http_parse_test.cc
#include "agentflow/net/http_parse.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace agentflow::net {
namespace {

TEST(ParseUrlTest, HttpsDefaultsToPort443) {
  auto u = ParseUrl("https://api.openai.com/v1/chat/completions");
  ASSERT_TRUE(u.ok()) << u.status().message();
  EXPECT_EQ(u->host, "api.openai.com");
  EXPECT_EQ(u->port, "443");
  EXPECT_EQ(u->target, "/v1/chat/completions");
  EXPECT_TRUE(u->tls);
}

TEST(ParseUrlTest, ExplicitPortAndPlainHttp) {
  auto u = ParseUrl("http://127.0.0.1:11434/v1/chat/completions");
  ASSERT_TRUE(u.ok());
  EXPECT_EQ(u->host, "127.0.0.1");
  EXPECT_EQ(u->port, "11434");
  EXPECT_EQ(u->target, "/v1/chat/completions");
  EXPECT_FALSE(u->tls);
}

TEST(ParseUrlTest, MissingPathBecomesRoot) {
  auto u = ParseUrl("https://example.com");
  ASSERT_TRUE(u.ok());
  EXPECT_EQ(u->target, "/");
}

TEST(ParseUrlTest, RejectsUnsupportedScheme) {
  EXPECT_FALSE(ParseUrl("ftp://example.com/x").ok());
  EXPECT_FALSE(ParseUrl("example.com/x").ok());
}

TEST(ParseResponseHeadTest, ParsesStatusAndHeadersCaseInsensitively) {
  const std::string raw =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "body-starts-here";
  auto h = ParseResponseHead(raw);
  ASSERT_TRUE(h.ok());
  EXPECT_EQ(h->status_code, 200);
  EXPECT_TRUE(h->chunked);
  EXPECT_EQ(raw.substr(h->head_bytes), "body-starts-here");
}

TEST(ParseResponseHeadTest, ReportsContentLengthWhenNotChunked) {
  auto h = ParseResponseHead(
      "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 17\r\n\r\n");
  ASSERT_TRUE(h.ok());
  EXPECT_EQ(h->status_code, 429);
  EXPECT_FALSE(h->chunked);
  EXPECT_EQ(h->content_length, 17);
}

TEST(ParseResponseHeadTest, IncompleteHeadIsNotAnError) {
  // The terminator has not arrived yet — the caller must read more bytes.
  auto h = ParseResponseHead("HTTP/1.1 200 OK\r\nContent-Ty");
  ASSERT_TRUE(h.ok());
  EXPECT_EQ(h->head_bytes, 0u);  // 0 = "not complete yet"
}

TEST(ChunkedDecoderTest, DecodesChunksSplitAcrossFeeds) {
  ChunkedDecoder d;
  auto a = d.Feed("5\r\nhel");
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(*a, "hel");
  auto b = d.Feed("lo\r\n0\r\n\r\n");
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(*b, "lo");
  EXPECT_TRUE(d.complete());
}

TEST(ChunkedDecoderTest, HandlesAChunkSizeLineSplitMidNumber) {
  // Chunk sizes are HEX (RFC 9112 §7.1), so the "10" assembled across these
  // two feeds means SIXTEEN bytes, not ten. The payload below is 16 bytes.
  ChunkedDecoder d;
  auto a = d.Feed("1");
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(*a, "");
  auto b = d.Feed("0\r\n0123456789abcdef\r\n0\r\n\r\n");
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(*b, "0123456789abcdef");
  EXPECT_TRUE(d.complete());
}

TEST(SseFramerTest, SplitsFramesOnBlankLine) {
  SseFramer f;
  auto got = f.Feed("data: {\"a\":1}\n\ndata: {\"b\":2}\n\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})", R"({"b":2})"}));
  EXPECT_FALSE(f.saw_done());
}

TEST(SseFramerTest, HoldsAPartialFrameUntilItCompletes) {
  SseFramer f;
  EXPECT_TRUE(f.Feed("data: {\"a\":").empty());
  auto got = f.Feed("1}\n\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})"}));
}

TEST(SseFramerTest, DoneSentinelIsConsumedNotDelivered) {
  SseFramer f;
  auto got = f.Feed("data: {\"a\":1}\n\ndata: [DONE]\n\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})"}));
  EXPECT_TRUE(f.saw_done());
}

TEST(SseFramerTest, IgnoresCommentsAndEventLines) {
  SseFramer f;
  auto got = f.Feed(": keep-alive\n\nevent: ping\ndata: {\"a\":1}\n\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})"}));
}

TEST(SseFramerTest, ToleratesCrLfLineEndings) {
  SseFramer f;
  auto got = f.Feed("data: {\"a\":1}\r\n\r\n");
  EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})"}));
}

}  // namespace
}  // namespace agentflow::net
