// tests/unit/tools/mcp_tool_adapter_test.cc
#include "agentflow/tools/mcp_tool_adapter.h"

#include <chrono>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/tool.h"
#include "tests/unit/tools/fake_mcp_server.h"

using namespace std::chrono_literals;

namespace agentflow::mcp {
namespace {

using ::agentflow::testing::FakeMcpClient;
using json = nlohmann::json;

ToolSchema MakeSchema(std::string name) {
  return ToolSchema{.name = std::move(name),
                    .description = "test",
                    .params_json_schema = R"({"type":"object"})"};
}

TEST(McpToolAdapterTest, InvokeReturnsResult) {
  asio::io_context io;
  auto client = std::make_shared<FakeMcpClient>(io);
  client->on_call_tool = [](std::string_view name, std::string_view args) {
    EXPECT_EQ(name, "echo");
    return std::string("ok:") + std::string(args);
  };
  McpToolAdapter adapter(client, "echo", MakeSchema("echo"));

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await adapter.Invoke("{\"x\":1}", "", CancelToken{});
      },
      asio::use_future);
  io.run();
  EXPECT_EQ(fut.get(), R"(ok:{"x":1})");
}

TEST(McpToolAdapterTest, InvokeServerErrorWrapsMcpErrorJson) {
  asio::io_context io;
  auto client = std::make_shared<FakeMcpClient>(io);
  client->on_call_tool = [](std::string_view, std::string_view) {
    return absl::InternalError("server blew up");
  };
  McpToolAdapter adapter(client, "boom", MakeSchema("boom"));

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await adapter.Invoke("{}", "", CancelToken{});
      },
      asio::use_future);
  io.run();
  auto out = json::parse(fut.get());
  EXPECT_TRUE(out.value("isError", false));
  ASSERT_TRUE(out["content"].is_array());
  EXPECT_NE(std::string(out["content"][0]["text"]).find("server blew up"),
            std::string::npos);
}

// Regression: nlohmann's parse_error::what() can cut a multi-byte UTF-8
// character in half (its "last read" context is sliced by bytes). Wrapping
// such a message with strict dump() used to throw type_error.316 and hide the
// original error. The adapter must still return a valid error JSON.
TEST(McpToolAdapterTest, InvokeErrorWithInvalidUtf8StillYieldsValidJson) {
  asio::io_context io;
  auto client = std::make_shared<FakeMcpClient>(io);
  client->on_call_tool = [](std::string_view, std::string_view) {
    return absl::InvalidArgumentError(
        std::string("args_json parse error: [json.exception.parse_error.101] "
                    "parse error: \"query\": \xE5' tail"));
  };
  McpToolAdapter adapter(client, "search", MakeSchema("search"));

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await adapter.Invoke("{}", "", CancelToken{});
      },
      asio::use_future);
  io.run();
  auto out = json::parse(fut.get());
  EXPECT_TRUE(out.value("isError", false));
  ASSERT_TRUE(out["content"].is_array());
  EXPECT_NE(std::string(out["content"][0]["text"]).find("args_json parse error"),
            std::string::npos);
}

TEST(McpToolAdapterTest, InvokeErrorKeepsValidUtf8Text) {
  asio::io_context io;
  auto client = std::make_shared<FakeMcpClient>(io);
  client->on_call_tool = [](std::string_view, std::string_view) {
    return absl::InternalError("搜索失败：尾帧");
  };
  McpToolAdapter adapter(client, "search", MakeSchema("search"));

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await adapter.Invoke("{}", "", CancelToken{});
      },
      asio::use_future);
  io.run();
  auto out = json::parse(fut.get());
  EXPECT_NE(std::string(out["content"][0]["text"]).find("尾帧"),
            std::string::npos);
}

TEST(McpToolAdapterTest, InvokeTimeoutWrapsMcpErrorJson) {
  asio::io_context io;
  auto client = std::make_shared<FakeMcpClient>(io);
  client->call_delay = 500ms;
  client->on_call_tool = [](std::string_view, std::string_view) {
    return std::string("never reached");
  };
  McpToolAdapter adapter(client, "slow", MakeSchema("slow"), 50ms);

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await adapter.Invoke("{}", "", CancelToken{});
      },
      asio::use_future);
  io.run();
  auto out = json::parse(fut.get());
  EXPECT_TRUE(out.value("isError", false));
  EXPECT_NE(std::string(out["content"][0]["text"]).find("timeout"),
            std::string::npos);
}

TEST(McpToolAdapterTest, InvokePreCancelledShortCircuits) {
  asio::io_context io;
  auto client = std::make_shared<FakeMcpClient>(io);
  client->on_call_tool = [](std::string_view, std::string_view) {
    ADD_FAILURE() << "CallTool should not be invoked when pre-cancelled";
    return std::string{};
  };
  McpToolAdapter adapter(client, "x", MakeSchema("x"));
  CancelSource src;
  src.Cancel();

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_return co_await adapter.Invoke("{}", "", src.Token());
      },
      asio::use_future);
  io.run();
  auto out = json::parse(fut.get());
  EXPECT_TRUE(out.value("isError", false));
  EXPECT_EQ(client->call_calls.load(), 0);
}

TEST(McpToolAdapterTest, SchemaPassThrough) {
  asio::io_context io;
  auto client = std::make_shared<FakeMcpClient>(io);
  auto schema = MakeSchema("hello");
  schema.description = "greets you";
  McpToolAdapter adapter(client, "hello", schema);
  EXPECT_EQ(adapter.Schema().name, "hello");
  EXPECT_EQ(adapter.Schema().description, "greets you");
}

}  // namespace
}  // namespace agentflow::mcp
