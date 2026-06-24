// tests/integration/tools/mcp_namespace_smoke_test.cc
//
// Drives the real stdio echo MCP server and verifies AttachMcpServer's
// name_prefix namespacing: the tool registers as "<prefix>.echo" while the
// remote tools/call still receives the raw name "echo".

#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include <absl/status/status.h>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/tool_registry.h"
#include "mcp_spec.pb.h"

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

proto::McpServerSpec EchoSpec() {
  proto::McpServerSpec s;
  s.set_transport(proto::McpServerSpec::STDIO);
  s.set_command_or_url("/usr/bin/python3");
  s.add_args(EchoServerPath());
  return s;
}

TEST(McpNamespaceSmokeTest, PrefixNamespacesToolAndCallsRawRemote) {
  asio::io_context io;
  ToolRegistry reg(io);

  bool attach_ok = false;
  std::string invoke_out;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto st = co_await reg.AttachMcpServer(EchoSpec(), "remote");
        attach_ok = st.ok();
        if (attach_ok) {
          CancelToken cancel;
          invoke_out = co_await reg.Invoke("mcp__remote__echo",
                                           R"({"text":"hi-there"})", cancel);
        }
        reg.ShutdownMcp();
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_TRUE(attach_ok);
  EXPECT_TRUE(reg.Has("mcp__remote__echo"));
  EXPECT_FALSE(reg.Has("echo"));
  EXPECT_NE(invoke_out.find("hi-there"), std::string::npos);
}

}  // namespace
}  // namespace agentflow
