// tests/unit/tools/mcp_client_pool_test.cc
#include "agentflow/tools/mcp_client_pool.h"

#include <memory>
#include <unordered_set>

#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "mcp_spec.pb.h"
#include "tests/unit/tools/fake_mcp_server.h"

namespace agentflow::mcp {
namespace {

using ::agentflow::testing::FakeMcpClient;

proto::McpServerSpec MakeSpec(proto::McpServerSpec::Transport t,
                              std::string cmd,
                              std::vector<std::string> args = {}) {
  proto::McpServerSpec s;
  s.set_transport(t);
  s.set_command_or_url(std::move(cmd));
  for (auto& a : args) s.add_args(std::move(a));
  return s;
}

McpClientPool::ClientFactory FakeFactory(
    std::shared_ptr<std::vector<std::shared_ptr<FakeMcpClient>>> tape) {
  return [tape](const proto::McpServerSpec& /*spec*/,
                asio::io_context& io) -> std::shared_ptr<IMcpClient> {
    auto c = std::make_shared<FakeMcpClient>(io);
    tape->push_back(c);
    return c;
  };
}

TEST(McpClientPoolTest, SameSpecReturnsSameClient) {
  asio::io_context io;
  auto tape = std::make_shared<std::vector<std::shared_ptr<FakeMcpClient>>>();
  McpClientPool pool(io, FakeFactory(tape));

  auto spec = MakeSpec(proto::McpServerSpec::STDIO, "/bin/cat");
  auto a = pool.GetOrCreate(spec);
  auto b = pool.GetOrCreate(spec);
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(pool.size(), 1u);
  EXPECT_EQ(tape->size(), 1u);
}

TEST(McpClientPoolTest, DifferingCommandReturnsDifferentClient) {
  asio::io_context io;
  auto tape = std::make_shared<std::vector<std::shared_ptr<FakeMcpClient>>>();
  McpClientPool pool(io, FakeFactory(tape));

  auto a = pool.GetOrCreate(
      MakeSpec(proto::McpServerSpec::STDIO, "/bin/cat"));
  auto b = pool.GetOrCreate(
      MakeSpec(proto::McpServerSpec::STDIO, "/bin/yes"));
  EXPECT_NE(a.get(), b.get());
  EXPECT_EQ(pool.size(), 2u);
}

TEST(McpClientPoolTest, FilterFieldsDoNotAffectKey) {
  asio::io_context io;
  auto tape = std::make_shared<std::vector<std::shared_ptr<FakeMcpClient>>>();
  McpClientPool pool(io, FakeFactory(tape));

  auto base = MakeSpec(proto::McpServerSpec::STDIO, "/bin/cat");
  auto with_filter = base;
  with_filter.add_include_tools("only-this");
  with_filter.set_call_timeout_ms(1234);
  with_filter.set_lazy_start(true);

  auto a = pool.GetOrCreate(base);
  auto b = pool.GetOrCreate(with_filter);
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(pool.size(), 1u);
}

TEST(McpClientPoolTest, SameHeadersReturnSameClient) {
  asio::io_context io;
  auto tape = std::make_shared<std::vector<std::shared_ptr<FakeMcpClient>>>();
  McpClientPool pool(io, FakeFactory(tape));

  auto make = [] {
    auto s = MakeSpec(proto::McpServerSpec::HTTP_SSE, "http://mcp.test/x");
    (*s.mutable_headers())["Authorization"] = "Bearer tok-a";
    (*s.mutable_headers())["X-Trace"] = "abc";
    return s;
  };

  auto a = pool.GetOrCreate(make());
  auto b = pool.GetOrCreate(make());
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(pool.size(), 1u);
}

TEST(McpClientPoolTest, DifferentAuthorizationReturnsDifferentClient) {
  asio::io_context io;
  auto tape = std::make_shared<std::vector<std::shared_ptr<FakeMcpClient>>>();
  McpClientPool pool(io, FakeFactory(tape));

  auto base = MakeSpec(proto::McpServerSpec::HTTP_SSE, "http://mcp.test/x");
  auto spec_a = base;
  (*spec_a.mutable_headers())["Authorization"] = "Bearer tok-a";
  auto spec_b = base;
  (*spec_b.mutable_headers())["Authorization"] = "Bearer tok-b";

  auto a = pool.GetOrCreate(spec_a);
  auto b = pool.GetOrCreate(spec_b);
  EXPECT_NE(a.get(), b.get());
  EXPECT_EQ(pool.size(), 2u);
}

// proto::Map's iteration order is UNSPECIFIED and can depend on insertion
// order. CanonicalKey must sort entries before folding them into the key, or
// two specs carrying the SAME headers -- just inserted in a DIFFERENT order
// -- could produce different keys and silently defeat dedup only some of the
// time. Insert several entries in reverse order between the two specs here
// to exercise exactly that trap.
TEST(McpClientPoolTest, SameHeadersDifferentInsertionOrderReturnSameClient) {
  asio::io_context io;
  auto tape = std::make_shared<std::vector<std::shared_ptr<FakeMcpClient>>>();
  McpClientPool pool(io, FakeFactory(tape));

  auto spec_a = MakeSpec(proto::McpServerSpec::HTTP_SSE, "http://mcp.test/x");
  auto& ha = *spec_a.mutable_headers();
  ha["Authorization"] = "Bearer tok";
  ha["X-A"] = "1";
  ha["X-B"] = "2";
  ha["X-C"] = "3";
  ha["X-D"] = "4";

  auto spec_b = MakeSpec(proto::McpServerSpec::HTTP_SSE, "http://mcp.test/x");
  auto& hb = *spec_b.mutable_headers();
  hb["X-D"] = "4";
  hb["X-C"] = "3";
  hb["X-B"] = "2";
  hb["X-A"] = "1";
  hb["Authorization"] = "Bearer tok";

  auto a = pool.GetOrCreate(spec_a);
  auto b = pool.GetOrCreate(spec_b);
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(pool.size(), 1u);
}

TEST(McpClientPoolTest, ClearShutsDownAll) {
  asio::io_context io;
  auto tape = std::make_shared<std::vector<std::shared_ptr<FakeMcpClient>>>();
  McpClientPool pool(io, FakeFactory(tape));

  pool.GetOrCreate(MakeSpec(proto::McpServerSpec::STDIO, "/bin/cat"));
  pool.GetOrCreate(MakeSpec(proto::McpServerSpec::STDIO, "/bin/yes"));
  ASSERT_EQ(tape->size(), 2u);
  pool.Clear();
  for (auto& c : *tape) {
    EXPECT_GE(c->shutdown_calls.load(), 1);
  }
  EXPECT_EQ(pool.size(), 0u);
}

}  // namespace
}  // namespace agentflow::mcp
