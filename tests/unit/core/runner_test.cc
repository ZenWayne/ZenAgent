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
  // Cancelled run must not produce a terminal state — the slow node returned
  // its (unmodified) state on cancel, but the runner skips fan-out for a
  // cancelled node, so no terminal state is recorded.
  State out = fut.get();
  EXPECT_TRUE(out.IsEmpty());
}

TEST(RunnerTest, CancelDoesNotFireDownstreamNodes) {
  auto counter = std::make_shared<std::atomic<int>>(0);
  auto downstream_ran = std::make_shared<std::atomic<bool>>(false);
  auto mark_ran = [downstream_ran](State&) {
    downstream_ran->store(true);
  };

  // upstream sleeps 5s (long enough to be cancelled mid-flight); downstream
  // would only run if the runner naively fanned out from a cancelled node.
  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("upstream", 5s, counter, nullptr))
   .AddNode(std::make_unique<StubNode>("downstream", 0ms, counter, mark_ran))
   .AddEdge("upstream", "downstream");

  CancelSource src;
  asio::io_context io;
  Runner runner(b.Build(), Runner::Options{});
  auto fut = asio::co_spawn(io,
    [&]() -> asio::awaitable<State> {
      co_return co_await runner.Run(MakeInitState(), src.Token());
    },
    asio::use_future);

  std::thread t([&] { io.run(); });
  std::this_thread::sleep_for(50ms);
  src.Cancel();
  t.join();

  State out = fut.get();
  EXPECT_FALSE(downstream_ran->load());
  EXPECT_TRUE(out.IsEmpty());
}

TEST(RunnerTest, GraphDoneEmittedOnSuccess) {
  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("only", 0ms, nullptr, nullptr));

  CapturingEmitter cap;
  (void)RunSync(b.Build(), Runner::Options{.trace = &cap}, MakeInitState());

  ASSERT_FALSE(cap.events.empty());
  const auto& last = cap.events.back();
  EXPECT_EQ(last.kind(), proto::TraceEvent::GRAPH_DONE);
  ASSERT_TRUE(last.has_graph_done());
  EXPECT_FALSE(last.graph_done().failed());
}

TEST(RunnerTest, GraphDoneEmittedOnFailure) {
  auto throwing = [](State&) { throw ToolError("boom"); };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("a", 0ms, nullptr, throwing));

  CapturingEmitter cap;
  EXPECT_THROW(
      RunSync(b.Build(), Runner::Options{.trace = &cap}, MakeInitState()),
      ToolError);

  ASSERT_FALSE(cap.events.empty());
  const auto& last = cap.events.back();
  EXPECT_EQ(last.kind(), proto::TraceEvent::GRAPH_DONE);
  ASSERT_TRUE(last.has_graph_done());
  EXPECT_TRUE(last.graph_done().failed());
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

// ---- activation_group self-cycle test --------------------------------------
// Adapted from autogen's test_digraph_group_chat_loop_with_self_cycle:
//
//   A -> B -> B (self-loop, activation_group="B_loop")
//   B -> C  (autogen routes via condition="exit"; P1 has no edge-conditional
//            routing, so we model the exit as a body-thrown exception after
//            three loop iterations)
//
// Expected source order before exit: ["A", "B", "B", "B"].

TEST(RunnerTest, SelfCycleAgentBLoopsThenExits) {
  auto sources = std::make_shared<std::vector<std::string>>();
  auto sources_mu = std::make_shared<std::mutex>();

  auto record_a = [sources, sources_mu](State&) {
    std::lock_guard<std::mutex> lk(*sources_mu);
    sources->push_back("A");
  };

  auto loop_b_or_exit = [sources, sources_mu](State& s) {
    {
      std::lock_guard<std::mutex> lk(*sources_mu);
      sources->push_back("B");
    }
    auto& ts = s.Mutable<test::TestState>();
    ts.set_counter(ts.counter() + 1);
    // Equivalent to autogen's condition flipping from "loop" to "exit" after
    // three iterations of B. Body throw stands in for the exit branch.
    if (ts.counter() >= 3) throw AgentflowError("exit");
  };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("A", 0ms, nullptr, record_a))
   .AddNode(std::make_unique<StubNode>("B", 0ms, nullptr, loop_b_or_exit))
   .AddEdge("A", "B")
   .AddEdge("B", "B", /*user_group=*/1, Edge::Condition::ALL);

  EXPECT_THROW(RunSync(b.Build(), Runner::Options{}, MakeInitState()),
               AgentflowError);

  ASSERT_EQ(sources->size(), 4u);
  EXPECT_EQ((*sources)[0], "A");
  EXPECT_EQ((*sources)[1], "B");
  EXPECT_EQ((*sources)[2], "B");
  EXPECT_EQ((*sources)[3], "B");
}

}  // namespace
}  // namespace agentflow
