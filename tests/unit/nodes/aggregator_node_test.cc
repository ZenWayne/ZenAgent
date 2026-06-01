// tests/unit/nodes/aggregator_node_test.cc
#include "agentflow/nodes/aggregator_node.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/state.h"
#include "test_messages.pb.h"

namespace agentflow {
namespace {

State MakeState(int counter, std::string last) {
  test::TestState s;
  s.set_counter(counter);
  s.set_last_node(std::move(last));
  return State::From(std::move(s));
}

TEST(AggregatorNodeTest, NullMergerIsIdentity) {
  AggregatorNodeConfig cfg;
  cfg.id = "agg";
  AggregatorNode node(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node.Run(MakeState(7, "x"), CancelToken{}, emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  EXPECT_EQ(out.As<test::TestState>().counter(), 7);
  EXPECT_EQ(out.As<test::TestState>().last_node(), "x");
}

TEST(AggregatorNodeTest, CustomMergerTransforms) {
  AggregatorNodeConfig cfg;
  cfg.id = "agg";
  cfg.merger = [](State s) {
    auto& m = s.Mutable<test::TestState>();
    m.set_counter(m.counter() + 1);
    m.set_last_node("agg");
    return s;
  };
  AggregatorNode node(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node.Run(MakeState(7, "x"), CancelToken{}, emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  EXPECT_EQ(out.As<test::TestState>().counter(), 8);
  EXPECT_EQ(out.As<test::TestState>().last_node(), "agg");
}

}  // namespace
}  // namespace agentflow
