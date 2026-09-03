// tests/unit/tools/tavily_tools_test.cc
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "examples/deep-search/tavily_tools.h"
#include "tests/support/fake_http_client.h"

namespace agentflow {
namespace {

using json = nlohmann::json;
using testing::FakeHttpClient;
using testing::FakeHttpTurn;
using net::HttpRequest;

// Runs one tool invocation on `io` and returns the result string.
std::string Invoke(std::shared_ptr<Tool> tool, std::string args,
                   asio::io_context& io) {
  CancelSource cancel;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await tool->Invoke(args, "", cancel.Token());
      },
      asio::use_future);
  io.run();
  return fut.get();
}

TEST(TavilyToolsTest, SearchSendsBearerAndParsesResults) {
  asio::io_context io;
  std::vector<FakeHttpTurn> turns(1);
  turns[0].status_code = 200;
  turns[0].body =
      R"({"query":"q","results":[{"title":"T","url":"https://e.com",)"
      R"("content":"C"}],"answer":null})";
  FakeHttpClient fake(std::move(turns));
  auto tool = deep_search::MakeTavilySearchTool(fake, "tvly-TEST");

  std::string out = Invoke(tool, R"({"query":"who is leo messi"})", io);
  json parsed = json::parse(out);
  ASSERT_TRUE(parsed.is_array());
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0]["title"], "T");
  EXPECT_EQ(parsed[0]["url"], "https://e.com");
  EXPECT_EQ(parsed[0]["content"], "C");

  // Request shape: URL, bearer header, and a body with the right fields.
  const std::vector<HttpRequest>& reqs = fake.requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].url, "https://api.tavily.com/search");
  bool has_bearer = false;
  for (const auto& h : reqs[0].headers) {
    if (h.first == "Authorization") {
      has_bearer = h.second == "Bearer tvly-TEST";
    }
  }
  EXPECT_TRUE(has_bearer);
  json body = json::parse(reqs[0].body);
  EXPECT_EQ(body["query"], "who is leo messi");
  EXPECT_EQ(body["max_results"], 5);
}

TEST(TavilyToolsTest, ExtractTrimsAndKeepsFailedResults) {
  asio::io_context io;
  std::vector<FakeHttpTurn> turns(1);
  turns[0].status_code = 200;
  turns[0].body =
      R"({"results":[{"url":"https://a.com","raw_content":"hello"}],)"
      R"("failed_results":[{"url":"https://b.com","error":"403"}]})";
  FakeHttpClient fake(std::move(turns));
  auto tool = deep_search::MakeTavilyExtractTool(fake, "tvly-TEST");

  std::string out = Invoke(tool, R"({"urls":["https://a.com","https://b.com"]})",
                           io);
  json parsed = json::parse(out);
  ASSERT_TRUE(parsed.contains("results"));
  EXPECT_EQ(parsed["results"][0]["url"], "https://a.com");
  EXPECT_EQ(parsed["results"][0]["raw_content"], "hello");
  ASSERT_TRUE(parsed.contains("failed_results"));
  EXPECT_EQ(parsed["failed_results"][0]["url"], "https://b.com");

  const std::vector<HttpRequest>& reqs = fake.requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].url, "https://api.tavily.com/extract");
  json body = json::parse(reqs[0].body);
  ASSERT_EQ(body["urls"].size(), 2u);
  EXPECT_EQ(body["urls"][0], "https://a.com");
}

TEST(TavilyToolsTest, HttpErrorSurfacesAsErrorJson) {
  asio::io_context io;
  std::vector<FakeHttpTurn> turns(1);
  turns[0].status_code = 401;
  turns[0].status = absl::UnauthenticatedError("unauthorized");
  FakeHttpClient fake(std::move(turns));
  auto tool = deep_search::MakeTavilySearchTool(fake, "tvly-BAD");

  std::string out = Invoke(tool, R"({"query":"x"})", io);
  json parsed = json::parse(out);
  EXPECT_TRUE(parsed.contains("error"));
}

}  // namespace
}  // namespace agentflow
