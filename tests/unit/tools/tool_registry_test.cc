// tests/unit/tools/tool_registry_test.cc
#include "agentflow/tools/tool_registry.h"
#include "agentflow/tools/native_fn_tool.h"

#include <gtest/gtest.h>

namespace agentflow {
namespace {

std::shared_ptr<NativeFnTool> MakeEcho() {
  return std::make_shared<NativeFnTool>(
      ToolSchema{.name = "echo", .description = "echo",
                 .params_json_schema = R"({"type":"object"})"},
      [](std::string_view args, const CancelToken&) -> asio::awaitable<std::string> {
        co_return std::string(args);
      });
}

TEST(ToolRegistryTest, RegisterAndInvoke) {
  auto reg = std::make_shared<ToolRegistry>();
  reg->Register(MakeEcho());

  asio::io_context io;
  auto fut = asio::co_spawn(io,
      [reg]() -> asio::awaitable<void> {
        auto result = co_await reg->Invoke("echo", "\"test\"", CancelToken{});
        EXPECT_EQ(result, "\"test\"");
      },
      asio::use_future);
  io.run();
  fut.get();
}

TEST(ToolRegistryTest, InvokeUnknownThrows) {
  auto reg = std::make_shared<ToolRegistry>();
  asio::io_context io;
  auto fut = asio::co_spawn(io,
      [reg]() -> asio::awaitable<void> {
        EXPECT_THROW(
            co_await reg->Invoke("nobody", "{}", CancelToken{}),
            AgentflowError);
      },
      asio::use_future);
  io.run();
  fut.get();
}

TEST(ToolRegistryTest, ExportJson) {
  auto reg = std::make_shared<ToolRegistry>();
  reg->Register(MakeEcho());

  std::vector<std::string> names = {"echo"};
  std::string json = reg->ExportToolsJson(names);
  EXPECT_NE(json.find("echo"), std::string::npos);
  EXPECT_NE(json.find("function"), std::string::npos);

  auto parsed = nlohmann::json::parse(json);
  ASSERT_TRUE(parsed.is_array());
  ASSERT_EQ(parsed.size(), 1);
  EXPECT_EQ(parsed[0]["function"]["name"], "echo");
}

}  // namespace
}  // namespace agentflow
