// tests/integration/tools/stdio_mcp_smoke_test.cc
//
// Drives the real McpClient over stdio against a tiny Python echo MCP
// server living next to this file. Validates that the JSON-RPC framing /
// request-response correlation / handshake / tools-discovery / Invoke
// path all work end-to-end against an actual child process.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>

#include "absl/status/status.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/tool_registry.h"
#include "mcp_spec.pb.h"

namespace agentflow {
namespace {

using json = nlohmann::json;

// Returns the absolute path to echo_mcp_server.py inside the Bazel runfiles
// tree. TEST_SRCDIR layout under Bzlmod is "<srcdir>/_main/<pkg>".
std::string EchoServerPath() {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  if (srcdir != nullptr) {
    std::string candidate =
        std::string(srcdir) +
        "/_main/tests/integration/tools/echo_mcp_server.py";
    struct stat st;
    if (::stat(candidate.c_str(), &st) == 0) return candidate;
  }
  return "tests/integration/tools/echo_mcp_server.py";
}

proto::McpServerSpec EchoSpec() {
  proto::McpServerSpec s;
  s.set_transport(proto::McpServerSpec::STDIO);
  s.set_command_or_url("/usr/bin/python3");
  s.add_args(EchoServerPath());
  return s;
}

TEST(StdioMcpSmokeTest, AttachAndInvokeEcho) {
  asio::io_context io;
  ToolRegistry reg(io);

  // Capture both the attach status and the invoke result (or thrown error)
  // so a failed attach surfaces with a useful message instead of std::terminate.
  struct Outcome {
    absl::Status attach_status;
    bool invoke_threw = false;
    std::string invoke_error;
    std::string invoke_result;
    std::string export_listing;
  } outcome;

  std::string io_error;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        try {
          outcome.attach_status = co_await reg.AttachMcpServer(EchoSpec());
          std::vector<std::string> ask = {"echo"};
          outcome.export_listing = reg.ExportToolsJson(ask);
        } catch (const std::exception& e) {
          outcome.attach_status =
              absl::InternalError(std::string("attach/list threw: ") +
                                  e.what());
          co_return;
        }
        try {
          outcome.invoke_result = co_await reg.Invoke(
              "echo", R"({"text":"hi from agentflow"})", "", CancelToken{});
        } catch (const std::exception& e) {
          outcome.invoke_threw = true;
          outcome.invoke_error = e.what();
        }
        // Tear down the MCP client so its detached ReadLoop unblocks and
        // io.run() returns once the rest of the work is drained.
        reg.ShutdownMcp();
      },
      asio::detached);

  try {
    io.run();
  } catch (const std::exception& e) {
    io_error = e.what();
  }
  ASSERT_TRUE(io_error.empty()) << "io.run() threw: " << io_error;

  ASSERT_TRUE(outcome.attach_status.ok())
      << "AttachMcpServer failed: " << outcome.attach_status.message()
      << " (python path: " << EchoServerPath() << ")";
  EXPECT_NE(outcome.export_listing.find("\"echo\""), std::string::npos)
      << "tool list missing echo; got: " << outcome.export_listing;
  ASSERT_FALSE(outcome.invoke_threw)
      << "Invoke threw: " << outcome.invoke_error;

  auto parsed = json::parse(outcome.invoke_result);
  ASSERT_TRUE(parsed["content"].is_array());
  ASSERT_FALSE(parsed["content"].empty());
  EXPECT_EQ(parsed["content"][0]["text"], "hi from agentflow");
}

}  // namespace
}  // namespace agentflow
