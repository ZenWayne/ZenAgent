// examples/core-stub-graph/main.cc
//
// Demo for P1: a 5-node graph with stub nodes that simulate work via short
// sleeps. The runner schedules them honoring activation groups.
//
//   start ──> planner ──> worker ──> reviewer ──> sink

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/graph.h"
#include "agentflow/core/runner.h"
#include "agentflow/core/state.h"
#include "agentflow/observability/jsonl_event_emitter.h"
#include "test_messages.pb.h"

namespace af = agentflow;
using namespace std::chrono_literals;

class DemoNode : public af::Node {
 public:
  DemoNode(std::string id, std::chrono::milliseconds delay)
      : id_(std::move(id)), delay_(delay) {}

  std::string_view Id() const override { return id_; }
  std::string_view Kind() const override { return "demo"; }

  asio::awaitable<af::State> Run(af::State state, const af::CancelToken& cancel,
                                  af::EventEmitter& emit) override {
    emit.EmitNodeStart(id_);
    auto exec = co_await asio::this_coro::executor;
    asio::steady_timer t(exec, delay_);
    co_await t.async_wait(asio::use_awaitable);
    auto& ts = state.Mutable<agentflow::test::TestState>();
    ts.set_counter(ts.counter() + 1);
    ts.set_last_node(id_);
    emit.EmitNodeEnd(id_, false, false);
    (void)cancel;
    co_return std::move(state);
  }

 private:
  std::string id_;
  std::chrono::milliseconds delay_;
};

int main() {
  af::GraphBuilder b;
  b.AddNode(std::make_unique<DemoNode>("start", 5ms))
   .AddNode(std::make_unique<DemoNode>("planner", 30ms))
   .AddNode(std::make_unique<DemoNode>("worker", 50ms))
   .AddNode(std::make_unique<DemoNode>("reviewer", 20ms))
   .AddNode(std::make_unique<DemoNode>("sink", 5ms))
   .AddEdge("start", "planner")
   .AddEdge("planner", "worker")
   .AddEdge("worker", "reviewer")
   .AddEdge("reviewer", "sink");

  auto graph = b.Build();
  std::cout << "GRAPH:\n" << graph.ToDotString() << "---\n";

  agentflow::test::TestState init;
  init.mutable_query()->set_text("demo");
  init.set_counter(0);

  af::JsonlEventEmitter emit(std::cout);
  af::Runner runner(std::move(graph), af::Runner::Options{.trace = &emit});

  asio::io_context io;
  auto fut = asio::co_spawn(io,
    [&]() -> asio::awaitable<af::State> {
      co_return co_await runner.Run(af::State::From(init));
    },
    asio::use_future);
  io.run();
  auto out = fut.get();

  const auto& ts = out.As<agentflow::test::TestState>();
  std::cout << "---\nfinal: last_node=" << ts.last_node()
            << " counter=" << ts.counter() << "\n";
  return ts.counter() == 5 && ts.last_node() == "sink" ? 0 : 1;
}
