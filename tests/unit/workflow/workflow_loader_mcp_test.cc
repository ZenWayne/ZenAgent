// tests/unit/workflow/workflow_loader_mcp_test.cc
//
// Parse/validation tests for JSON-declared mcp_servers. These exercise paths
// that fail before (or without) a successful MCP connection, so no subprocess
// or echo server is needed. Real-connection behavior lives in the integration
// test tests/integration/tools/loader_mcp_smoke_test.cc.
#include "agentflow/workflow/workflow_loader.h"

#include <memory>
#include <string>

#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow.h"

namespace agentflow::workflow {
namespace {

absl::StatusOr<std::shared_ptr<Workflow>> RunLoadAndAttach(
    asio::io_context& io, ToolRegistry& reg, std::string_view json) {
  absl::StatusOr<std::shared_ptr<Workflow>> result;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        result = co_await WorkflowLoader::LoadAndAttach(json, reg);
        co_return;
      },
      asio::detached);
  io.run();
  return result;
}

TEST(WorkflowLoaderMcpTest, DuplicateServerIdRejected) {
  asio::io_context io;
  ToolRegistry reg(io);
  constexpr char kDup[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[
      {"id":"x","transport":"stdio","command_or_url":"/a"},
      {"id":"x","transport":"stdio","command_or_url":"/b"}],
    "agents":{"c":{"system_prompt":"h","tools":[]}},"main":"c"})";
  auto r = RunLoadAndAttach(io, reg, kDup);
  ASSERT_FALSE(r.ok());
  EXPECT_TRUE(absl::StrContains(r.status().message(), "duplicate"));
}

TEST(WorkflowLoaderMcpTest, DottedServerIdRejected) {
  asio::io_context io;
  ToolRegistry reg(io);
  constexpr char kDot[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[{"id":"a.b","transport":"stdio","command_or_url":"/a"}],
    "agents":{"c":{"system_prompt":"h","tools":[]}},"main":"c"})";
  auto r = RunLoadAndAttach(io, reg, kDot);
  ASSERT_FALSE(r.ok());
  EXPECT_TRUE(absl::StrContains(r.status().message(), "."));
}

TEST(WorkflowLoaderMcpTest, UndeclaredPrefixIsHardError) {
  asio::io_context io;
  ToolRegistry reg(io);
  // No servers declared; agent references 'ghost.echo' → CheckReferences error.
  constexpr char kJson[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[],
    "agents":{"chat":{"system_prompt":"h","tools":["ghost.echo"]}},"main":"chat"})";
  auto r = RunLoadAndAttach(io, reg, kJson);
  ASSERT_FALSE(r.ok());
  EXPECT_TRUE(absl::StrContains(r.status().message(), "unknown tool"));
}

TEST(WorkflowLoaderMcpTest, SyncLoadRejectsMcpServers) {
  asio::io_context io;
  ToolRegistry host(io);
  constexpr char kJson[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[{"id":"fs","transport":"stdio","command_or_url":"/x"}],
    "agents":{"chat":{"system_prompt":"h","tools":[]}},"main":"chat"})";
  auto result = WorkflowLoader::Load(kJson, host);
  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "LoadAndAttach"));
}

TEST(WorkflowLoaderMcpTest, FailedServerDropsItsToolsAndBuilds) {
  asio::io_context io;
  ToolRegistry reg(io);
  // 'fs' points at a non-existent command → connect fails → degrade.
  // Agent references fs.echo → tool dropped, workflow still builds.
  constexpr char kJson[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[{"id":"fs","transport":"stdio",
                    "command_or_url":"/nonexistent/mcp-server-xyz"}],
    "agents":{"chat":{"system_prompt":"h","tools":["fs.echo"]}},"main":"chat"})";
  auto r = RunLoadAndAttach(io, reg, kJson);
  ASSERT_TRUE(r.ok()) << r.status().message();
  EXPECT_FALSE(reg.Has("fs.echo"));
}

}  // namespace
}  // namespace agentflow::workflow
