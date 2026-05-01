// tests/unit/core/runner_test.cc
#include "agentflow/core/runner.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/graph.h"
#include "tests/unit/core/stub_node.h"
#include "test_messages.pb.h"

namespace agentflow {
namespace {

using namespace std::chrono_literals;

class CapturingEmitter : public EventEmitter {
 public:
  void Emit(proto::TraceEvent ev) override {
    std::lock_guard<std::mutex> l(m_);
    events.push_back(std::move(ev));
  }
  std::vector<proto::TraceEvent> events;
  std::mutex m_;
};

State MakeInitState() {
  test::TestState s;
  s.set_counter(0);
  return State::From(s);
}

State RunSync(Graph g, Runner::Options opts, State initial,
              CancelToken cancel = CancelToken()) {
  asio::io_context io;
  Runner runner(std::move(g), opts);
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await runner.Run(std::move(initial), cancel);
      },
      asio::use_future);
  io.run();
  return fut.get();
}

TEST(RunnerTest, LinearGraphRunsInOrder) {
  // RunSync blocks until the runner returns, so stack-local order/order_mu
  // outlive every lambda invocation; reference capture is safe.
  std::vector<std::string> order;
  std::mutex order_mu;
  auto record_for = [&](std::string node_id) {
    return [&, node_id](State& s) {
      {
        std::lock_guard<std::mutex> lk(order_mu);
        order.push_back(node_id);
      }
      s.Mutable<test::TestState>().set_counter(
          s.As<test::TestState>().counter() + 1);
    };
  };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("a", 0ms, nullptr, record_for("a")))
   .AddNode(std::make_unique<StubNode>("b", 0ms, nullptr, record_for("b")))
   .AddNode(std::make_unique<StubNode>("c", 0ms, nullptr, record_for("c")))
   .AddEdge("a", "b").AddEdge("b", "c");

  CapturingEmitter cap;
  auto out = RunSync(b.Build(), Runner::Options{.trace = &cap}, MakeInitState());

  EXPECT_EQ(out.As<test::TestState>().counter(), 3);
  EXPECT_EQ(order, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(RunnerTest, DiamondFanInMergesLastWriter) {
  auto counter = std::make_shared<std::atomic<int>>(0);

  auto setName = [](std::string n) {
    return [n](State& s) {
      s.Mutable<test::TestState>().set_last_node(n);
    };
  };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("entry", 0ms, counter, setName("entry")))
   .AddNode(std::make_unique<StubNode>("left",  10ms, counter, setName("left")))
   .AddNode(std::make_unique<StubNode>("right", 20ms, counter, setName("right")))
   .AddNode(std::make_unique<StubNode>("sink",  0ms, counter, nullptr))
   .AddEdge("entry", "left").AddEdge("entry", "right")
   .AddEdge("left", "sink").AddEdge("right", "sink");

  auto out = RunSync(b.Build(), Runner::Options{}, MakeInitState());

  // Sink got merged inputs; right finishes after left, so its writer wins
  // under last-writer policy.
  EXPECT_EQ(out.As<test::TestState>().last_node(), "right");
}

TEST(RunnerTest, CycleStopsAtMaxIterationsViaBody) {
  // Body increments a counter. Self-loop tagged with user_group=1 so the
  // runner treats it as a cycle (resetting between firings + bootstrap on
  // the first firing). The body throws after counter reaches 3 to terminate.
  auto counter = std::make_shared<std::atomic<int>>(0);

  auto inc_or_throw = [](State& s) {
    auto& ts = s.Mutable<test::TestState>();
    ts.set_counter(ts.counter() + 1);
    if (ts.counter() >= 3) throw AgentflowError("done after 3");
  };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("entry", 0ms, counter, nullptr))
   .AddNode(std::make_unique<StubNode>("loop", 0ms, counter, inc_or_throw))
   .AddEdge("entry", "loop")
   .AddEdge("loop", "loop", /*user_group=*/1, Edge::Condition::ALL);

  EXPECT_THROW(
      RunSync(b.Build(), Runner::Options{}, MakeInitState()),
      AgentflowError);
}

TEST(RunnerTest, CancelTerminatesPromptly) {
  auto counter = std::make_shared<std::atomic<int>>(0);

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("slow", 5s, counter, nullptr));
  auto g = b.Build();

  CancelSource src;
  asio::io_context io;
  Runner runner(std::move(g), Runner::Options{});
  auto start = std::chrono::steady_clock::now();
  auto fut = asio::co_spawn(io,
    [&]() -> asio::awaitable<State> {
      co_return co_await runner.Run(MakeInitState(), src.Token());
    },
    asio::use_future);

  std::thread t([&] { io.run(); });
  std::this_thread::sleep_for(50ms);
  src.Cancel();
  t.join();
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, 500ms);
  (void)fut.get();
}

TEST(RunnerTest, NodeFailureAbortsGraph) {
  auto counter = std::make_shared<std::atomic<int>>(0);

  auto throwing = [](State&) { throw ToolError("oops"); };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("a", 0ms, counter, throwing))
   .AddNode(std::make_unique<StubNode>("b", 0ms, counter, nullptr))
   .AddEdge("a", "b");

  EXPECT_THROW(RunSync(b.Build(), Runner::Options{}, MakeInitState()),
               ToolError);
}

}  // namespace
}  // namespace agentflow
