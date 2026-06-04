// tests/unit/core/runner_test.cc
#include "agentflow/core/runner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/graph.h"
#include "agentflow/core/stub_node.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "agentflow/persist/checkpoint_writer.h"
#include "checkpoint.pb.h"
#include "test_messages.pb.h"

namespace agentflow {
namespace {

using namespace std::chrono_literals;

// Thin test helper: wraps CallbackEventEmitter so tests can read back
// captured events without re-implementing an EventEmitter subclass.
struct EventCapture {
  std::vector<proto::TraceEvent> events;
  std::mutex m;
  CallbackEventEmitter emitter{[this](const proto::TraceEvent& e) {
    std::lock_guard<std::mutex> l(m);
    events.push_back(e);
  }};
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

State ResumeSync(Graph g, Runner::Options opts, const proto::Checkpoint& cp,
                 State target) {
  asio::io_context io;
  Runner runner(std::move(g), opts);
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<State> {
        co_return co_await runner.Resume(cp, std::move(target));
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

  EventCapture cap;
  auto out = RunSync(b.Build(), Runner::Options{.trace = &cap.emitter}, MakeInitState());

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

  EventCapture cap;
  (void)RunSync(b.Build(), Runner::Options{.trace = &cap.emitter}, MakeInitState());

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

  EventCapture cap;
  EXPECT_THROW(
      RunSync(b.Build(), Runner::Options{.trace = &cap.emitter}, MakeInitState()),
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

// ── NODE_START / NODE_END emission ─────────────────────────────────────────

TEST(RunnerTest, EmitsNodeStartEndForLinearGraph) {
  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("a", 0ms, nullptr, nullptr))
   .AddNode(std::make_unique<StubNode>("b", 0ms, nullptr, nullptr))
   .AddEdge("a", "b");

  EventCapture cap;
  (void)RunSync(b.Build(), Runner::Options{.trace = &cap.emitter},
                MakeInitState());

  std::vector<std::pair<int, std::string>> seq;
  for (const auto& e : cap.events) {
    seq.emplace_back(e.kind(), e.node_id());
  }
  // Expected: NODE_START a, NODE_END a, EDGE_FIRE (a->b), NODE_START b,
  // NODE_END b, GRAPH_DONE.
  ASSERT_EQ(seq.size(), 6u);
  EXPECT_EQ(seq[0].first, proto::TraceEvent::NODE_START);
  EXPECT_EQ(seq[0].second, "a");
  EXPECT_EQ(seq[1].first, proto::TraceEvent::NODE_END);
  EXPECT_EQ(seq[1].second, "a");
  EXPECT_FALSE(cap.events[1].node_end().cancelled());
  EXPECT_FALSE(cap.events[1].node_end().failed());
  EXPECT_EQ(seq[2].first, proto::TraceEvent::EDGE_FIRE);
  EXPECT_EQ(seq[3].first, proto::TraceEvent::NODE_START);
  EXPECT_EQ(seq[3].second, "b");
  EXPECT_EQ(seq[4].first, proto::TraceEvent::NODE_END);
  EXPECT_EQ(seq[5].first, proto::TraceEvent::GRAPH_DONE);
}

TEST(RunnerTest, EmitsNodeEndWithFailedFlag) {
  auto throwing = [](State&) { throw ToolError("boom"); };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("a", 0ms, nullptr, throwing));

  EventCapture cap;
  EXPECT_THROW(RunSync(b.Build(), Runner::Options{.trace = &cap.emitter},
                       MakeInitState()),
               ToolError);

  // Sequence: NODE_START, NODE_FAILED, NODE_END(failed=1), GRAPH_DONE(failed=1).
  ASSERT_GE(cap.events.size(), 4u);
  EXPECT_EQ(cap.events[0].kind(), proto::TraceEvent::NODE_START);
  EXPECT_EQ(cap.events[1].kind(), proto::TraceEvent::NODE_FAILED);
  ASSERT_EQ(cap.events[2].kind(), proto::TraceEvent::NODE_END);
  EXPECT_TRUE(cap.events[2].node_end().failed());
  EXPECT_FALSE(cap.events[2].node_end().cancelled());
}

TEST(RunnerTest, EmitsNodeEndWithCancelledFlag) {
  CancelSource src;
  auto cancel_self = [&src](State&) { src.Cancel(); };

  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("a", 0ms, nullptr, cancel_self));

  EventCapture cap;
  (void)RunSync(b.Build(), Runner::Options{.trace = &cap.emitter},
                MakeInitState(), src.Token());

  // NODE_END for "a" carries cancelled=1.
  auto it = std::find_if(cap.events.begin(), cap.events.end(),
                         [](const proto::TraceEvent& e) {
                           return e.kind() == proto::TraceEvent::NODE_END &&
                                  e.node_id() == "a";
                         });
  ASSERT_NE(it, cap.events.end());
  EXPECT_TRUE(it->node_end().cancelled());
  EXPECT_FALSE(it->node_end().failed());
}

// ── Checkpoint hook / Resume ────────────────────────────────────────────────

// Test-only CheckpointWriter that captures every Write into a vector.
struct RecordingWriter : public CheckpointWriter {
  std::vector<proto::Checkpoint> writes;
  std::mutex m;
  absl::Status Write(const proto::Checkpoint& cp) override {
    std::lock_guard<std::mutex> l(m);
    writes.push_back(cp);
    return absl::OkStatus();
  }
};

namespace {
std::unique_ptr<StubNode> BumpCounter(std::string id) {
  return std::make_unique<StubNode>(
      std::move(id), 0ms, nullptr, [](State& s) {
        auto& m = s.Mutable<test::TestState>();
        m.set_counter(m.counter() + 1);
      });
}
}  // namespace

TEST(RunnerCheckpointTest, NoWriteWhenPolicyIsOff) {
  GraphBuilder b;
  b.AddNode(BumpCounter("a")).AddNode(BumpCounter("b")).AddEdge("a", "b");

  RecordingWriter w;
  Runner::Options opts;
  opts.checkpoint_writer = &w;  // present but policy=Off
  opts.checkpoint_policy = Runner::CheckpointPolicy::Off;

  (void)RunSync(b.Build(), opts, MakeInitState());
  EXPECT_TRUE(w.writes.empty());
}

TEST(RunnerCheckpointTest, WritesAfterEachNode) {
  GraphBuilder b;
  b.AddNode(BumpCounter("a"))
   .AddNode(BumpCounter("b"))
   .AddNode(BumpCounter("c"))
   .AddEdge("a", "b").AddEdge("b", "c");

  RecordingWriter w;
  Runner::Options opts;
  opts.checkpoint_policy = Runner::CheckpointPolicy::AfterEachNode;
  opts.checkpoint_writer = &w;

  auto out = RunSync(b.Build(), opts, MakeInitState());
  EXPECT_EQ(out.As<test::TestState>().counter(), 3);

  ASSERT_EQ(w.writes.size(), 3u);
  EXPECT_EQ(w.writes[0].completed_nodes_size(), 1);
  EXPECT_EQ(w.writes[0].completed_nodes(0), "a");
  EXPECT_EQ(w.writes[1].completed_nodes_size(), 2);
  EXPECT_EQ(w.writes[1].completed_nodes(1), "b");
  EXPECT_EQ(w.writes[2].completed_nodes_size(), 3);
  EXPECT_EQ(w.writes[2].completed_nodes(2), "c");
  // state_type stamped by Runner from the message's GetTypeName.
  EXPECT_EQ(w.writes[2].state_type(), "agentflow.test.TestState");
}

TEST(RunnerCheckpointTest, AfterTerminalNodeWritesOnce) {
  GraphBuilder b;
  b.AddNode(BumpCounter("a")).AddNode(BumpCounter("b")).AddEdge("a", "b");

  RecordingWriter w;
  Runner::Options opts;
  opts.checkpoint_policy = Runner::CheckpointPolicy::AfterTerminalNode;
  opts.checkpoint_writer = &w;

  (void)RunSync(b.Build(), opts, MakeInitState());
  ASSERT_EQ(w.writes.size(), 1u);
  EXPECT_EQ(w.writes[0].completed_nodes_size(), 2);
  EXPECT_EQ(w.writes[0].completed_nodes(1), "b");
}

TEST(RunnerCheckpointTest, FailingNodeIsNotRecorded) {
  GraphBuilder b;
  b.AddNode(BumpCounter("a"))
   .AddNode(std::make_unique<StubNode>("b", 0ms, nullptr,
                                       [](State&) { throw ToolError("boom"); }))
   .AddEdge("a", "b");

  RecordingWriter w;
  Runner::Options opts;
  opts.checkpoint_policy = Runner::CheckpointPolicy::AfterEachNode;
  opts.checkpoint_writer = &w;

  EXPECT_THROW(RunSync(b.Build(), opts, MakeInitState()), ToolError);
  // Only "a" should have been recorded; "b" threw and wasn't.
  ASSERT_EQ(w.writes.size(), 1u);
  ASSERT_EQ(w.writes[0].completed_nodes_size(), 1);
  EXPECT_EQ(w.writes[0].completed_nodes(0), "a");
}

TEST(RunnerCheckpointTest, ResumeContinuesFromMidGraph) {
  // Run a→b→c, capture the checkpoint written after b, then build a fresh
  // Runner with the same graph and Resume from that checkpoint. Verify only
  // c runs (counter only increments once in the resume) and final state
  // matches a clean run.
  GraphBuilder b1;
  b1.AddNode(BumpCounter("a"))
    .AddNode(BumpCounter("b"))
    .AddNode(BumpCounter("c"))
    .AddEdge("a", "b").AddEdge("b", "c");

  RecordingWriter w;
  Runner::Options opts;
  opts.checkpoint_policy = Runner::CheckpointPolicy::AfterEachNode;
  opts.checkpoint_writer = &w;

  auto clean = RunSync(b1.Build(), opts, MakeInitState());
  ASSERT_EQ(clean.As<test::TestState>().counter(), 3);
  ASSERT_GE(w.writes.size(), 2u);
  const proto::Checkpoint& after_b = w.writes[1];
  ASSERT_EQ(after_b.completed_nodes_size(), 2);
  EXPECT_EQ(after_b.completed_nodes(1), "b");

  // Fresh Runner with the same graph topology — Resume from after_b.
  GraphBuilder b2;
  b2.AddNode(BumpCounter("a"))
    .AddNode(BumpCounter("b"))
    .AddNode(BumpCounter("c"))
    .AddEdge("a", "b").AddEdge("b", "c");
  asio::io_context io;
  Runner r2(b2.Build(), Runner::Options{});

  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        co_return co_await r2.Resume(after_b, State::From(test::TestState{}));
      },
      asio::use_future);
  io.run();
  auto resumed = fut.get();

  // resumed.counter should equal after_b's counter + 1 (only c ran).
  // after_b's state had counter==2 (a and b each bumped from 0). So resumed
  // should be 3.
  test::TestState saved_state;
  ASSERT_TRUE(saved_state.ParseFromString(after_b.state_bytes()));
  EXPECT_EQ(saved_state.counter(), 2);
  EXPECT_EQ(resumed.As<test::TestState>().counter(), 3);
}

TEST(RunnerCheckpointTest, ResumeRejectsUnknownNodeInCheckpoint) {
  GraphBuilder b;
  b.AddNode(BumpCounter("a")).AddNode(BumpCounter("b")).AddEdge("a", "b");

  proto::Checkpoint cp;
  test::TestState s;
  cp.set_state_bytes(s.SerializeAsString());
  cp.set_state_type("agentflow.test.TestState");
  cp.add_completed_nodes("ghost");

  asio::io_context io;
  Runner r(b.Build(), Runner::Options{});
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        co_return co_await r.Resume(cp, State::From(test::TestState{}));
      },
      asio::use_future);
  io.run();
  EXPECT_THROW({ (void)fut.get(); }, AgentflowError);
}

TEST(RunnerCheckpointTest, ResumeWithAllCompletedReturnsTarget) {
  GraphBuilder b;
  b.AddNode(BumpCounter("a"));

  proto::Checkpoint cp;
  test::TestState s;
  s.set_counter(42);
  cp.set_state_bytes(s.SerializeAsString());
  cp.set_state_type("agentflow.test.TestState");
  cp.add_completed_nodes("a");

  asio::io_context io;
  Runner r(b.Build(), Runner::Options{});
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        co_return co_await r.Resume(cp, State::From(test::TestState{}));
      },
      asio::use_future);
  io.run();
  auto out = fut.get();
  // Counter stays at 42 — a never re-ran because it was already completed.
  EXPECT_EQ(out.As<test::TestState>().counter(), 42);
}

TEST(RunnerCheckpointTest, ResumeRejectsMalformedStateBytes) {
  auto counter = std::make_shared<std::atomic<int>>(0);
  GraphBuilder b;
  b.AddNode(std::make_unique<StubNode>("a", 0ms, counter, nullptr));

  proto::Checkpoint cp;
  cp.set_state_type("agentflow.test.TestState");
  cp.set_state_bytes("\xFF\xFF\xFF not valid TestState wire bytes");

  EXPECT_THROW(
      ResumeSync(b.Build(), Runner::Options{}, cp,
                 State::Empty<test::TestState>()),
      AgentflowError);
}

}  // namespace
}  // namespace agentflow
