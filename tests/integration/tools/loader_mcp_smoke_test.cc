// tests/integration/tools/loader_mcp_smoke_test.cc
//
// End-to-end: a workflow JSON declares the stdio echo MCP server; LoadAndAttach
// connects it, namespaces its tool as "echo.echo", and an agent referencing it
// resolves cleanly. Also verifies lazy_start is ignored (eager connect still
// registers tools).

#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>

#include <absl/status/statusor.h>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow.h"
#include "agentflow/workflow/workflow_loader.h"

namespace agentflow {
namespace {

std::string EchoServerPath() {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  if (srcdir != nullptr) {
    std::string candidate = std::string(srcdir) +
        "/_main/tests/integration/tools/echo_mcp_server.py";
    struct stat st;
    if (::stat(candidate.c_str(), &st) == 0) return candidate;
  }
  return "tests/integration/tools/echo_mcp_server.py";
}

// Builds a workflow JSON that declares the echo server (id "echo") with the
// given extra entry fields, and one agent referencing "echo.echo".
std::string MakeJson(const std::string& extra_server_fields) {
  return std::string(R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{"user_query":{"type":"string"}}},
    "mcp_servers":[{"id":"echo","transport":"stdio",
                    "command_or_url":"/usr/bin/python3","args":[")") +
      EchoServerPath() + R"("])" + extra_server_fields + R"(}],
    "agents":{"chat":{"system_prompt":"hi","tools":["echo.echo"]}},
    "main":"chat"})";
}

absl::StatusOr<std::shared_ptr<workflow::Workflow>> RunLoad(
    asio::io_context& io, ToolRegistry& reg, const std::string& json) {
  absl::StatusOr<std::shared_ptr<workflow::Workflow>> result;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        result = co_await workflow::WorkflowLoader::LoadAndAttach(json, reg);
        reg.ShutdownMcp();
        co_return;
      },
      asio::detached);
  io.run();
  return result;
}

TEST(LoaderMcpSmokeTest, LoadAndAttachStdioEcho) {
  asio::io_context io;
  ToolRegistry reg(io);
  auto result = RunLoad(io, reg, MakeJson(""));
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_TRUE(reg.Has("echo.echo"));
}

TEST(LoaderMcpSmokeTest, LazyStartIgnoredStillRegisters) {
  asio::io_context io;
  ToolRegistry reg(io);
  auto result = RunLoad(io, reg, MakeJson(R"(,"lazy_start":true)"));
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_TRUE(reg.Has("echo.echo"));  // eager connect despite lazy_start
}

TEST(LoaderMcpSmokeTest, IncludeToolsKeepsListedTool) {
  asio::io_context io;
  ToolRegistry reg(io);
  auto result =
      RunLoad(io, reg, MakeJson(R"(,"include_tools":["echo"],"call_timeout_ms":5000)"));
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_TRUE(reg.Has("echo.echo"));
}

TEST(LoaderMcpSmokeTest, ExcludeToolsDropsListedTool) {
  asio::io_context io;
  ToolRegistry reg(io);
  // Build a dedicated JSON: echo server with exclude_tools, agent has no tools
  // (so CheckReferences doesn't hard-error on the excluded tool).
  // Note: use R"json(...)json" delimiters to avoid )" clashing with outer R"(.
  const std::string json =
      std::string(R"json({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{"user_query":{"type":"string"}}},
    "mcp_servers":[{"id":"echo","transport":"stdio",
                    "command_or_url":"/usr/bin/python3","args":[")json") +
      EchoServerPath() + R"json("],"exclude_tools":["echo"]}],
    "agents":{"chat":{"system_prompt":"hi","tools":[]}},
    "main":"chat"})json";
  auto result = RunLoad(io, reg, json);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_FALSE(reg.Has("echo.echo"));
}

}  // namespace
}  // namespace agentflow
