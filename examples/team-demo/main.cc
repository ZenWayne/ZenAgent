// examples/team-demo/main.cc
//
// P4 demo: a two-step graph that shows TeamNode's StateRouter and
// ParallelGather policies running against stub members (no LLM required).
//
//   entry → team_state_router (a, b, c via state) → team_parallel_gather (x, y, z) → sink
//
// Prints the trace events as the runner executes the graph, and the final
// State so you can see what each policy did.

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/event.h"
#include "agentflow/core/graph.h"
#include "agentflow/core/runner.h"
#include "agentflow/core/state.h"
#include "agentflow/core/stub_node.h"
#include "agentflow/nodes/team_node.h"
#include "agentflow/observability/jsonl_event_emitter.h"
#include "test_messages.pb.h"

namespace af = agentflow;
using namespace std::chrono_literals;

namespace {

// Stub Node that appends its id to TestState.last_node and bumps counter.
std::unique_ptr<af::Node> Member(std::string id) {
  return std::make_unique<af::StubNode>(
      std::move(id), 0ms, nullptr,
      [](af::State& s) {
        auto& m = s.Mutable<af::test::TestState>();
        m.set_last_node(m.last_node() + "[member]");
        m.set_counter(m.counter() + 1);
      });
}

}  // namespace

int main() {
  // ── Build a StateRouter team: pick member by reading user_query ─────────────
  af::TeamNodeConfig router_cfg;
  router_cfg.id = "router_team";
  router_cfg.policy = af::TeamNodeConfig::Policy::StateRouter;
  router_cfg.members.push_back(Member("a"));
  router_cfg.members.push_back(Member("b"));
  router_cfg.members.push_back(Member("c"));
  // The router fn just walks through the members once, in order, then stops.
  auto step = std::make_shared<int>(0);
  router_cfg.router = [step, n = router_cfg.members.size()](const af::State&) {
    if (*step >= static_cast<int>(n)) return std::string{};
    static const char* ids[] = {"a", "b", "c"};
    return std::string(ids[(*step)++]);
  };

  // ── Build a ParallelGather team with a concat aggregator ───────────────────
  af::TeamNodeConfig gather_cfg;
  gather_cfg.id = "gather_team";
  gather_cfg.policy = af::TeamNodeConfig::Policy::ParallelGather;
  gather_cfg.members.push_back(Member("x"));
  gather_cfg.members.push_back(Member("y"));
  gather_cfg.members.push_back(Member("z"));
  gather_cfg.aggregator = [](std::vector<af::State> outs) {
    af::test::TestState merged;
    for (auto& s : outs) {
      const auto& m = s.As<af::test::TestState>();
      merged.set_counter(merged.counter() + m.counter());
      merged.set_last_node(merged.last_node() + m.last_node() + "/");
    }
    return af::State::From(std::move(merged));
  };

  // ── Sink, just to give the graph an end ─────────────────────────────────────
  auto sink_body = [](af::State& s) {
    s.Mutable<af::test::TestState>().set_last_node(
        s.As<af::test::TestState>().last_node() + "(sink)");
  };

  af::GraphBuilder b;
  b.AddNode(std::make_unique<af::TeamNode>(std::move(router_cfg)))
   .AddNode(std::make_unique<af::TeamNode>(std::move(gather_cfg)))
   .AddNode(std::make_unique<af::StubNode>("sink", 0ms, nullptr,
                                            std::move(sink_body)))
   .AddEdge("router_team", "gather_team")
   .AddEdge("gather_team", "sink");
  auto graph = b.Build();
  std::cout << "GRAPH:\n" << graph.ToDotString() << "---\n";

  af::JsonlEventEmitter emit(std::cout);
  af::Runner runner(std::move(graph), af::Runner::Options{.trace = &emit});

  asio::io_context io;
  af::test::TestState init;
  init.set_user_query("hello");
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<af::State> {
        co_return co_await runner.Run(
            af::State::From(std::move(init)));
      },
      asio::use_future);
  io.run();
  auto out = fut.get();

  const auto& ts = out.As<af::test::TestState>();
  std::cout << "\n=== Final ===\n"
            << "counter=" << ts.counter() << "\n"
            << "last_node=" << ts.last_node() << "\n";
  return 0;
}
