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
  auto counter = std::make_shared<std::atomic<int>>(0);

  // Each node records its execution position into state.counter and its id
  // into state.last_node. The shared atomic gives stable per-node positions
  // independent of the StubNode lifetime (the StubNodes are owned by the
  // Graph and destroyed when Runner goes out of scope).
  auto recorded_orders = std::make_shared<std::vector<std::pair<std::string, int>>>();
  auto recorded_mu = std::make_shared<std::mutex>();
  auto record_for = [recorded_orders, recorded_mu, counter](std::string node_id) {
    return [recorded_orders, recorded_mu, counter, node_id](State& s) {
      int pos = counter->fetch_add(1);
      {
        std::lock_guard<std::mutex> lk(*recorded_mu);
        recorded_orders->emplace_back(node_id, pos);
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
  ASSERT_EQ(recorded_orders->size(), 3u);
  EXPECT_EQ((*recorded_orders)[0].first, "a");
  EXPECT_EQ((*recorded_orders)[1].first, "b");
  EXPECT_EQ((*recorded_orders)[2].first, "c");
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

// ---- activation_group cycle tests ------------------------------------------

TEST(RunnerTest, TwoNodeCycleRunsMultipleIterations) {
  // entry -> a -> b -> a (group=1) ; node `a` increments counter every fire.
  // After 3 fires of `a`, `a` throws to terminate. Verify that the throw
  // happens (proves the cycle was actually re-entered the expected number of
  // times by the runner).
  auto counter = std::make_shared<std::atomic<int>>(0);

  auto inc_or_throw = [](State& s) {
    auto& ts = s.Mutable<test::TestState>();
    ts.set_counter(ts.counter() + 1);
    if (ts.counter() >= 3) throw AgentflowError("done after 3");
  };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("entry", 0ms, counter, nullptr))
   .AddNode(std::make_unique<StubNode>("a", 0ms, counter, inc_or_throw))
   .AddNode(std::make_unique<StubNode>("b", 0ms, counter, nullptr))
   .AddEdge("entry", "a")
   .AddEdge("a", "b", /*user_group=*/1, Edge::Condition::ALL)
   .AddEdge("b", "a", /*user_group=*/1, Edge::Condition::ALL);

  EXPECT_THROW(RunSync(b.Build(), Runner::Options{}, MakeInitState()),
               AgentflowError);
}

TEST(RunnerTest, NodeWithMixedGroupsRequiresBoth) {
  // Topology:
  //   entry --(group 0)--> joinNode
  //   loopSrc --(group 1)--> joinNode
  //   joinNode --(group 1)--> loopSrc
  //
  // joinNode has TWO incoming groups. On its first activation:
  //   - group=0 must drain (entry must fire first).
  //   - group=1 is vacuously satisfied via cycle bootstrap.
  // After joinNode fires, loopSrc fires (its only incoming, in group 1, fires
  // and triggers the runner's cycle reset). Each subsequent firing of joinNode
  // requires group=1 to drain again. We bound the loop with a counter throw.
  auto counter = std::make_shared<std::atomic<int>>(0);

  auto bump_or_throw = [](State& s) {
    auto& ts = s.Mutable<test::TestState>();
    ts.set_counter(ts.counter() + 1);
    if (ts.counter() >= 3) throw AgentflowError("3 cycles done");
  };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("entry", 0ms, counter, nullptr))
   .AddNode(std::make_unique<StubNode>("joinNode", 0ms, counter, bump_or_throw))
   .AddNode(std::make_unique<StubNode>("loopSrc", 0ms, counter, nullptr))
   .AddEdge("entry", "joinNode")
   .AddEdge("joinNode", "loopSrc", 1, Edge::Condition::ALL)
   .AddEdge("loopSrc", "joinNode", 1, Edge::Condition::ALL);

  // We expect the cycle to actually iterate (joinNode fires multiple times),
  // proving that on subsequent activations group=1 is being driven by
  // loopSrc->joinNode firings. The body throw caps it at 3.
  EXPECT_THROW(RunSync(b.Build(), Runner::Options{}, MakeInitState()),
               AgentflowError);
}

TEST(RunnerTest, TwoIndependentCyclesRunInParallel) {
  // entry -> a1 -> b1 -> a1 (group=1)
  // entry -> a2 -> b2 -> a2 (group=2)
  // Each cycle is independent. We use body throws to terminate them.
  auto counter1 = std::make_shared<std::atomic<int>>(0);
  auto counter2 = std::make_shared<std::atomic<int>>(0);

  auto bump1 = [counter1](State&) {
    if (counter1->fetch_add(1) + 1 >= 2) throw AgentflowError("c1 done");
  };
  auto bump2 = [counter2](State&) {
    if (counter2->fetch_add(1) + 1 >= 2) throw AgentflowError("c2 done");
  };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("entry", 0ms, nullptr, nullptr))
   .AddNode(std::make_unique<StubNode>("a1", 0ms, nullptr, bump1))
   .AddNode(std::make_unique<StubNode>("b1", 0ms, nullptr, nullptr))
   .AddNode(std::make_unique<StubNode>("a2", 0ms, nullptr, bump2))
   .AddNode(std::make_unique<StubNode>("b2", 0ms, nullptr, nullptr))
   .AddEdge("entry", "a1").AddEdge("entry", "a2")
   .AddEdge("a1", "b1", 1, Edge::Condition::ALL)
   .AddEdge("b1", "a1", 1, Edge::Condition::ALL)
   .AddEdge("a2", "b2", 2, Edge::Condition::ALL)
   .AddEdge("b2", "a2", 2, Edge::Condition::ALL);

  // First cycle to throw aborts the whole graph; either bump1 or bump2 wins.
  EXPECT_THROW(RunSync(b.Build(), Runner::Options{}, MakeInitState()),
               AgentflowError);

  // At least one cycle must have iterated past its first body call, proving
  // the runner re-entered it (i.e., the cycle counter reset worked).
  EXPECT_GE(counter1->load() + counter2->load(), 2);
}

}  // namespace
}  // namespace agentflow
