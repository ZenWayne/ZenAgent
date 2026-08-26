// tests/unit/tools/tool_registry_mcp_test.cc
#include "agentflow/tools/tool_registry.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/tools/mcp_client_pool.h"
#include "agentflow/tools/native_fn_tool.h"
#include "agentflow/tools/tool.h"
#include "mcp_spec.pb.h"
#include "tests/unit/tools/fake_mcp_server.h"

namespace agentflow {
namespace {

using ::agentflow::testing::FakeMcpClient;

ToolSchema MakeRemoteSchema(std::string name) {
  return ToolSchema{.name = std::move(name),
                    .description = "remote",
                    .params_json_schema = R"({"type":"object"})"};
}

// Returns a factory that captures the most recently created FakeMcpClient so
// tests can configure its hooks/state. The factory is invoked synchronously
// during ToolRegistry::AttachMcpServer → pool->GetOrCreate, BEFORE Connect or
// ListTools is awaited; the test pre-stages the response via on_*.
struct FakeFactoryHandle {
  std::shared_ptr<FakeMcpClient> latest;
};

mcp::McpClientPool::ClientFactory MakeFactory(
    std::shared_ptr<FakeFactoryHandle> h,
    std::function<void(FakeMcpClient&)> configure) {
  return [h, configure = std::move(configure)](
             const proto::McpServerSpec& /*spec*/,
             asio::io_context& io) -> std::shared_ptr<mcp::IMcpClient> {
    auto c = std::make_shared<FakeMcpClient>(io);
    if (configure) configure(*c);
    h->latest = c;
    return c;
  };
}

proto::McpServerSpec MakeSpec(std::vector<std::string> include = {},
                              std::vector<std::string> exclude = {},
                              bool lazy = false) {
  proto::McpServerSpec s;
  s.set_transport(proto::McpServerSpec::STDIO);
  s.set_command_or_url("/usr/bin/fake-mcp");
  for (auto& x : include) s.add_include_tools(x);
  for (auto& x : exclude) s.add_exclude_tools(x);
  s.set_lazy_start(lazy);
  return s;
}

TEST(ToolRegistryMcpTest, AttachRegistersRemoteTools) {
  asio::io_context io;
  auto h = std::make_shared<FakeFactoryHandle>();
  ToolRegistry reg(io, MakeFactory(h, [](FakeMcpClient& c) {
    c.on_list_tools = [] {
      return std::vector<ToolSchema>{MakeRemoteSchema("a"),
                                     MakeRemoteSchema("b"),
                                     MakeRemoteSchema("c")};
    };
    c.on_call_tool = [](std::string_view name, std::string_view) {
      return std::string("called:") + std::string(name);
    };
  }));

  auto attach_fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<absl::Status> {
        co_return co_await reg.AttachMcpServer(MakeSpec());
      },
      asio::use_future);
  auto invoke_fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_await reg.AttachMcpServer(MakeSpec());  // idempotent on re-attach
        co_return co_await reg.Invoke("b", "{}", CancelToken{});
      },
      asio::use_future);
  io.run();
  EXPECT_TRUE(attach_fut.get().ok());
  EXPECT_EQ(invoke_fut.get(), "called:b");
}

TEST(ToolRegistryMcpTest, AttachExcludesByFilter) {
  asio::io_context io;
  auto h = std::make_shared<FakeFactoryHandle>();
  ToolRegistry reg(io, MakeFactory(h, [](FakeMcpClient& c) {
    c.on_list_tools = [] {
      return std::vector<ToolSchema>{MakeRemoteSchema("a"),
                                     MakeRemoteSchema("b")};
    };
  }));

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<absl::Status> {
        co_return co_await reg.AttachMcpServer(MakeSpec({"a"}));
      },
      asio::use_future);
  io.run();
  ASSERT_TRUE(fut.get().ok());

  std::vector<std::string> ask = {"a", "b"};
  auto json = reg.ExportToolsJson(ask);
  EXPECT_NE(json.find("\"a\""), std::string::npos);
  EXPECT_EQ(json.find("\"b\""), std::string::npos);  // excluded by include-only
}

TEST(ToolRegistryMcpTest, AttachUnreachableEagerFails) {
  asio::io_context io;
  auto h = std::make_shared<FakeFactoryHandle>();
  ToolRegistry reg(io, MakeFactory(h, [](FakeMcpClient& c) {
    c.on_connect = [] { return absl::UnavailableError("nope"); };
  }));

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<absl::Status> {
        co_return co_await reg.AttachMcpServer(MakeSpec({}, {}, false));
      },
      asio::use_future);
  io.run();
  auto status = fut.get();
  EXPECT_FALSE(status.ok());

  // No tools were registered.
  std::vector<std::string> empty;
  auto json = reg.ExportToolsJson(empty);
  EXPECT_EQ(json, "[]");
}

TEST(ToolRegistryMcpTest, AttachUnreachableLazySucceeds) {
  asio::io_context io;
  auto h = std::make_shared<FakeFactoryHandle>();
  ToolRegistry reg(io, MakeFactory(h, [](FakeMcpClient& c) {
    // lazy: Connect is NOT called eagerly. But ListTools fires, and we make
    // it fail to simulate the server being unreachable at attach time.
    c.on_list_tools = [] {
      return absl::UnavailableError("server gone");
    };
  }));

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<absl::Status> {
        co_return co_await reg.AttachMcpServer(MakeSpec({}, {}, true));
      },
      asio::use_future);
  io.run();
  EXPECT_TRUE(fut.get().ok());
  EXPECT_EQ(h->latest->connect_calls.load(), 0);  // lazy → no eager connect
}

TEST(ToolRegistryMcpTest, LocalToolWinsOnCollision) {
  asio::io_context io;
  auto h = std::make_shared<FakeFactoryHandle>();
  ToolRegistry reg(io, MakeFactory(h, [](FakeMcpClient& c) {
    c.on_list_tools = [] {
      return std::vector<ToolSchema>{MakeRemoteSchema("t")};
    };
    c.on_call_tool = [](std::string_view, std::string_view) {
      return std::string("from-mcp");
    };
  }));

  // Pre-register a native tool named "t"; the subsequent MCP attach must
  // NOT replace it.
  reg.Register(std::make_shared<NativeFnTool>(
      ToolSchema{.name = "t",
                 .description = "local",
                 .params_json_schema = R"({"type":"object"})"},
      [](std::string_view, const CancelToken&)
          -> asio::awaitable<std::string> {
        co_return std::string("from-native");
      }));

  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        co_await reg.AttachMcpServer(MakeSpec());
        co_return co_await reg.Invoke("t", "{}", CancelToken{});
      },
      asio::use_future);
  io.run();
  EXPECT_EQ(fut.get(), "from-native");
}

}  // namespace
}  // namespace agentflow
