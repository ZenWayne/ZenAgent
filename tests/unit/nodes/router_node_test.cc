// tests/unit/nodes/router_node_test.cc
#include "agentflow/nodes/router_node.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/cancel.h"
#include "agentflow/core/errors.h"
#include "agentflow/core/event.h"
#include "agentflow/core/state.h"
#include "test_messages.pb.h"

namespace agentflow {
namespace {

State MakeStateWithQuery(std::string q) {
  test::TestState s;
  s.set_user_query(std::move(q));
  return State::From(std::move(s));
}

TEST(RouterNodeTest, WritesChooserResultIntoOutputField) {
  RouterNodeConfig cfg;
  cfg.id = "r";
  cfg.output_field = "last_node";
  cfg.chooser = [](const State& s) {
    return s.As<test::TestState>().user_query() == "go-a" ? std::string("a")
                                                          : std::string("b");
  };
  RouterNode node(std::move(cfg));

  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node.Run(MakeStateWithQuery("go-a"), CancelToken{},
                                    emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  EXPECT_EQ(out.As<test::TestState>().last_node(), "a");
}

TEST(RouterNodeTest, CancelShortCircuits) {
  bool chooser_called = false;
  RouterNodeConfig cfg;
  cfg.id = "r";
  cfg.output_field = "last_node";
  cfg.chooser = [&](const State&) {
    chooser_called = true;
    return std::string("a");
  };
  RouterNode node(std::move(cfg));

  CancelSource src;
  src.Cancel();
  asio::io_context io;
  NullEventEmitter emit;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node.Run(MakeStateWithQuery("x"), src.Token(),
                                    emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  EXPECT_FALSE(chooser_called);
  EXPECT_EQ(out.As<test::TestState>().last_node(), "");  // untouched
}

TEST(RouterNodeTest, CtorRejectsMissingFields) {
  EXPECT_THROW(
      ({
        RouterNodeConfig c;
        c.chooser = [](const State&) { return std::string{}; };
        RouterNode r(std::move(c));
      }),
      AgentflowError);
  EXPECT_THROW(
      ({
        RouterNodeConfig c;
        c.id = "r";
        RouterNode r(std::move(c));
      }),
      AgentflowError);
}

}  // namespace
}  // namespace agentflow
