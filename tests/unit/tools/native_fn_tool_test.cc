// tests/unit/tools/native_fn_tool_test.cc
#include "agentflow/tools/native_fn_tool.h"

#include <gtest/gtest.h>

namespace agentflow {
namespace {

TEST(NativeFnToolTest, InvokeReturnsResult) {
  NativeFnTool tool(
      ToolSchema{.name = "echo", .description = "echo back",
                 .params_json_schema = "{}"},
      [](std::string_view args, const CancelToken&) -> asio::awaitable<std::string> {
        co_return std::string(args);
      });

  asio::io_context io;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<void> {
        auto result = co_await tool.Invoke("hello", CancelToken{});
        EXPECT_EQ(result, "hello");
      },
      asio::use_future);
  io.run();
  fut.get();
}

TEST(NativeFnToolTest, SchemaAccessible) {
  ToolSchema schema{"my_tool", "does stuff", R"({"type":"object"})"};
  NativeFnTool tool(schema, [](auto, auto) -> asio::awaitable<std::string> {
    co_return "";
  });
  EXPECT_EQ(tool.Schema().name, "my_tool");
}

}  // namespace
}  // namespace agentflow
