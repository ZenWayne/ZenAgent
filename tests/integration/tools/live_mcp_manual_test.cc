// tests/integration/tools/live_mcp_manual_test.cc
//
// Manual acceptance test against the REAL video-maker MCP server (not the
// Python echo fakes the other integration tests use). This exists to catch
// protocol details a fake server could accidentally paper over.
//
// THIS TEST IS READ-ONLY BY DESIGN AND MUST STAY THAT WAY:
//   - It only performs AttachMcpServer() + ExportToolsJson(), i.e. an MCP
//     tools/list. It MUST NEVER call Invoke() / tools/call.
//   - The server behind MCP_LIVE_URL is a real, running video-maker instance
//     backed by a user's real project data. A write tool (update_dialogue,
//     update_motion, ...) would really mutate that data; a generation tool
//     (start_generation, ...) would really incur billed API calls. Neither
//     is acceptable from an automated test that a developer might run
//     without thinking about it first.
//   - If you need to exercise an actual tools/call against the live server,
//     do it manually and limit yourself to a read-only, no-side-effect tool
//     such as get_authoring_guidelines — do not add that call here.
//
// This target is `tags = ["manual"]` in BUILD.bazel, so it never runs as
// part of `bazel test //tests/...`. It also skips itself (GTEST_SKIP) unless
// MCP_LIVE_URL is set, so even a direct `bazel test
// //tests/integration/tools:live_mcp_manual_test` run is a no-op without
// explicit opt-in. To actually run it against a live server:
//
//   MCP_LIVE_URL=http://localhost:8765/mcp
//     bazel test //tests/integration/tools:live_mcp_manual_test
//     --test_output=all
//
// Optionally set MCP_LIVE_BEARER to send an `Authorization: Bearer <token>`
// header (e.g. when going through an auth-enforcing gateway).
//
// Structure mirrors http_mcp_smoke_test.cc's AttachAndInvokeEcho test: same
// ToolRegistry + co_spawn + io.run() skeleton, same result-collection style.
// The only differences are: no Python subprocess is started, the URL/bearer
// come from the environment, and the assertion checks for real video-maker
// tool names instead of the fake server's "echo" tool.

#include <cstdlib>
#include <string>

#include "absl/status/status.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/tool_registry.h"
#include "mcp_spec.pb.h"

namespace agentflow {
namespace {

TEST(LiveMcpManualTest, ToolsListIncludesKnownVideoMakerTools) {
  const char* url = std::getenv("MCP_LIVE_URL");
  if (url == nullptr) {
    GTEST_SKIP() << "set MCP_LIVE_URL to run (e.g. http://localhost:8765/mcp)";
  }

  proto::McpServerSpec spec;
  spec.set_transport(proto::McpServerSpec::HTTP_SSE);
  spec.set_command_or_url(url);
  if (const char* bearer = std::getenv("MCP_LIVE_BEARER")) {
    (*spec.mutable_headers())["Authorization"] =
        std::string("Bearer ") + bearer;
  }

  asio::io_context io;
  ToolRegistry reg(io);

  struct Outcome {
    absl::Status attach_status;
    std::string export_listing;
  } outcome;

  std::string io_error;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        try {
          outcome.attach_status = co_await reg.AttachMcpServer(spec);
          // tools/list ONLY. Do not add Invoke()/tools-call here — see the
          // file-level comment for why.
          outcome.export_listing = reg.ExportToolsJson({});
        } catch (const std::exception& e) {
          outcome.attach_status = absl::InternalError(
              std::string("attach/list threw: ") + e.what());
        }
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
      << "AttachMcpServer failed: " << outcome.attach_status.message();
  EXPECT_NE(outcome.export_listing.find("\"get_shot\""), std::string::npos)
      << "tool list missing get_shot; got: " << outcome.export_listing;
  EXPECT_NE(outcome.export_listing.find("\"update_dialogue\""),
            std::string::npos)
      << "tool list missing update_dialogue; got: " << outcome.export_listing;
}

}  // namespace
}  // namespace agentflow
