// tests/unit/nodes/team_node_test.cc
#include "agentflow/nodes/team_node.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/core/errors.h"
#include "agentflow/core/event.h"
#include "agentflow/core/state.h"
#include "test_messages.pb.h"
#include "agentflow/core/stub_node.h"

using namespace std::chrono_literals;

namespace agentflow {
namespace {

State MakeState() { return State::From(test::TestState{}); }

// Convenience: a StubNode that appends "<id>;" to TestState.last_node and
// increments TestState.counter — useful for asserting which members ran in
// which order.
std::unique_ptr<Node> MakeAppender(std::string id,
                                   std::chrono::milliseconds delay = 0ms) {
  auto body = [id](State& s) {
    auto& m = s.Mutable<test::TestState>();
    m.set_last_node(m.last_node() + id + ";");
    m.set_counter(m.counter() + 1);
  };
  return std::make_unique<StubNode>(std::move(id), delay, nullptr,
                                    std::move(body));
}

// ── StateRouter ──────────────────────────────────────────────────────────────

TEST(TeamNodeTest, StateRouter_RouterDecidesOrderAndStops) {
  TeamNodeConfig cfg;
  cfg.id = "team";
  cfg.policy = TeamNodeConfig::Policy::StateRouter;
  cfg.members.push_back(MakeAppender("a"));
  cfg.members.push_back(MakeAppender("b"));
  auto counter = std::make_shared<int>(0);
  cfg.router = [counter](const State&) -> std::string {
    switch ((*counter)++) {
      case 0: return "a";
      case 1: return "b";
      default: return "";  // done
    }
  };
  TeamNode team(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await team.Run(MakeState(), CancelToken{}, emit);
      },
      asio::use_future);
  io.run();
  EXPECT_EQ(fut.get().As<test::TestState>().last_node(), "a;b;");
}

TEST(TeamNodeTest, StateRouter_StopsAtMaxTurns) {
  TeamNodeConfig cfg;
  cfg.id = "team";
  cfg.policy = TeamNodeConfig::Policy::StateRouter;
  cfg.members.push_back(MakeAppender("a"));
  cfg.max_turns = 3;
  cfg.router = [](const State&) -> std::string { return "a"; };
  TeamNode team(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await team.Run(MakeState(), CancelToken{}, emit);
      },
      asio::use_future);
  io.run();
  EXPECT_EQ(fut.get().As<test::TestState>().counter(), 3);
}

TEST(TeamNodeTest, StateRouter_UnknownMemberThrows) {
  TeamNodeConfig cfg;
  cfg.id = "team";
  cfg.policy = TeamNodeConfig::Policy::StateRouter;
  cfg.members.push_back(MakeAppender("a"));
  cfg.router = [](const State&) -> std::string { return "ghost"; };
  TeamNode team(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await team.Run(MakeState(), CancelToken{}, emit);
      },
      asio::use_future);
  io.run();
  EXPECT_THROW({ (void)fut.get(); }, AgentflowError);
}

TEST(TeamNodeTest, StateRouter_CancelExitsBetweenTurns) {
  TeamNodeConfig cfg;
  cfg.id = "team";
  cfg.policy = TeamNodeConfig::Policy::StateRouter;
  cfg.members.push_back(MakeAppender("a"));
  CancelSource src;
  cfg.router = [&src](const State&) -> std::string {
    src.Cancel();
    return "a";  // would run once before cancel is observed pre-turn next iter
  };
  TeamNode team(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await team.Run(MakeState(), src.Token(), emit);
      },
      asio::use_future);
  io.run();
  EXPECT_EQ(fut.get().As<test::TestState>().counter(), 1);  // exited next turn
}

// ── ParallelGather ───────────────────────────────────────────────────────────

TEST(TeamNodeTest, ParallelGather_DefaultLastWriterWins) {
  TeamNodeConfig cfg;
  cfg.id = "team";
  cfg.policy = TeamNodeConfig::Policy::ParallelGather;
  cfg.members.push_back(MakeAppender("a"));
  cfg.members.push_back(MakeAppender("b"));
  cfg.members.push_back(MakeAppender("c"));
  TeamNode team(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await team.Run(MakeState(), CancelToken{}, emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  // Each branch saw its own clone (counter 0→1, last_node "" → "<id>;").
  EXPECT_EQ(out.As<test::TestState>().counter(), 1);
  // Default = last-writer-wins; the order branches finish in is asio-impl
  // dependent but with no delay all three drain in submission order, so the
  // *last* state push by the gather is "c;".
  EXPECT_EQ(out.As<test::TestState>().last_node(), "c;");
}

TEST(TeamNodeTest, ParallelGather_AggregatorMerges) {
  TeamNodeConfig cfg;
  cfg.id = "team";
  cfg.policy = TeamNodeConfig::Policy::ParallelGather;
  cfg.members.push_back(MakeAppender("a"));
  cfg.members.push_back(MakeAppender("b"));
  cfg.aggregator = [](std::vector<State> outs) {
    test::TestState merged;
    for (auto& s : outs) {
      const auto& m = s.As<test::TestState>();
      merged.set_counter(merged.counter() + m.counter());
      merged.set_last_node(merged.last_node() + m.last_node());
    }
    return State::From(std::move(merged));
  };
  TeamNode team(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await team.Run(MakeState(), CancelToken{}, emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  EXPECT_EQ(out.As<test::TestState>().counter(), 2);  // 1 from each branch
  EXPECT_EQ(out.As<test::TestState>().last_node(), "a;b;");
}

TEST(TeamNodeTest, ParallelGather_MemberThrowSkipped) {
  TeamNodeConfig cfg;
  cfg.id = "team";
  cfg.policy = TeamNodeConfig::Policy::ParallelGather;
  cfg.members.push_back(MakeAppender("a"));
  cfg.members.push_back(std::make_unique<StubNode>(
      "boom", 0ms, nullptr,
      [](State&) { throw AgentflowError("intentional"); }));
  cfg.members.push_back(MakeAppender("c"));
  cfg.aggregator = [](std::vector<State> outs) {
    test::TestState merged;
    for (auto& s : outs) {
      merged.set_last_node(merged.last_node() +
                            s.As<test::TestState>().last_node());
    }
    return State::From(std::move(merged));
  };
  TeamNode team(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await team.Run(MakeState(), CancelToken{}, emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  // 'boom' was skipped; aggregator saw only a and c.
  EXPECT_EQ(out.As<test::TestState>().last_node(), "a;c;");
}

TEST(TeamNodeTest, ParallelGather_NullAggregatorEmptyMembersReturnsInput) {
  // Edge case the assertion in ctor guards against: members must be
  // non-empty. Verifies the ctor invariant rather than runtime behavior.
  TeamNodeConfig cfg;
  cfg.id = "team";
  cfg.policy = TeamNodeConfig::Policy::ParallelGather;
  EXPECT_THROW(TeamNode team(std::move(cfg)), AgentflowError);
}

// ── ctor / dispatch ──────────────────────────────────────────────────────────

TEST(TeamNodeTest, CtorRejectsMissingId) {
  TeamNodeConfig cfg;
  cfg.members.push_back(MakeAppender("a"));
  EXPECT_THROW(TeamNode team(std::move(cfg)), AgentflowError);
}

}  // namespace
}  // namespace agentflow
