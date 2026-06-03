// examples/checkpoint-demo/main.cc
//
// P7 acceptance demo. Linear graph: start → planner → researcher → writer.
// Each node bumps state.counter. AfterEachNode checkpoint policy writes
// /tmp/p7_demo.ckpt after every NODE_END.
//
// Modes:
//   (default) Run the graph, write checkpoint after each node, print final.
//   --kill-after <id>  After the named node's NODE_END (and its checkpoint
//                       write), std::abort the process. Simulates Demo B's
//                       `kill -9` after researcher.
//   --resume <path>     ReadCheckpointFromFile, Runner::Resume with the same
//                       graph topology. Skipped nodes don't re-run.
//
// Demo B round-trip:
//   $ ./checkpoint_demo --kill-after researcher
//      ... aborts.
//   $ ./checkpoint_demo --resume /tmp/p7_demo.ckpt
//      ... only writer runs; final counter == 4.
//
// Notes:
//   - To make --kill-after fire deterministically, every node sleeps 100ms
//     so the runner gets a chance to write checkpoints between nodes. The
//     abort fires from a one-shot probe coroutine that polls the on-disk
//     checkpoint, NOT from inside the node's Run — the checkpoint must
//     finish writing BEFORE we abort or the resume target is missing.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/graph.h"
#include "agentflow/core/runner.h"
#include "agentflow/core/state.h"
#include "agentflow/core/stub_node.h"
#include "agentflow/observability/jsonl_event_emitter.h"
#include "agentflow/persist/checkpoint_reader.h"
#include "agentflow/persist/file_checkpoint_writer.h"
#include "test_messages.pb.h"

namespace af = agentflow;
using namespace std::chrono_literals;

namespace {

constexpr char kDefaultCkpt[] = "/tmp/p7_demo.ckpt";

std::unique_ptr<af::Node> MakeNode(std::string id) {
  return std::make_unique<af::StubNode>(
      id, 100ms, nullptr, [id](af::State& s) {
        auto& m = s.Mutable<af::test::TestState>();
        m.set_counter(m.counter() + 1);
        m.set_last_node(id);
      });
}

af::Graph BuildGraph() {
  af::GraphBuilder b;
  b.AddNode(MakeNode("start"))
   .AddNode(MakeNode("planner"))
   .AddNode(MakeNode("researcher"))
   .AddNode(MakeNode("writer"))
   .AddEdge("start", "planner")
   .AddEdge("planner", "researcher")
   .AddEdge("researcher", "writer");
  return b.Build();
}

void PrintFinal(const af::State& out) {
  const auto& ts = out.As<af::test::TestState>();
  std::cout << "\n=== Final ===\n"
            << "counter=" << ts.counter() << "\n"
            << "last_node=" << ts.last_node() << "\n";
}

// Polls the on-disk checkpoint; once `target_node` shows up in
// completed_nodes, aborts the process — guaranteeing the checkpoint write
// finished before we kill.
asio::awaitable<void> KillAfterProbe(asio::io_context& io,
                                      std::string ckpt_path,
                                      std::string target_node) {
  asio::steady_timer t(io);
  while (true) {
    t.expires_after(25ms);
    co_await t.async_wait(asio::use_awaitable);
    auto cp = af::ReadCheckpointFromFile(ckpt_path);
    if (!cp.ok()) continue;
    for (const auto& id : cp->completed_nodes()) {
      if (id == target_node) {
        std::cout << "[demo] kill-after " << target_node
                  << " observed in checkpoint; aborting\n"
                  << std::flush;
        std::abort();
      }
    }
  }
}

int RunFresh(const std::string& kill_after, const std::string& ckpt_path) {
  // Wipe any prior checkpoint so the probe doesn't trigger on stale state.
  (void)std::remove(ckpt_path.c_str());

  af::FileCheckpointWriter writer(ckpt_path);
  af::Runner::Options opts;
  opts.checkpoint_policy = af::Runner::CheckpointPolicy::AfterEachNode;
  opts.checkpoint_writer = &writer;
  af::JsonlEventEmitter trace(std::cerr);
  opts.trace = &trace;

  af::Runner runner(BuildGraph(), opts);
  af::test::TestState init;
  init.set_counter(0);

  asio::io_context io;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<af::State> {
        co_return co_await runner.Run(af::State::From(init));
      },
      asio::use_future);

  if (!kill_after.empty()) {
    asio::co_spawn(io, KillAfterProbe(io, ckpt_path, kill_after),
                    asio::detached);
  }
  io.run();
  PrintFinal(fut.get());
  return 0;
}

int RunResume(const std::string& ckpt_path) {
  auto cp_or = af::ReadCheckpointFromFile(ckpt_path);
  if (!cp_or.ok()) {
    std::cerr << "ReadCheckpointFromFile failed: " << cp_or.status() << "\n";
    return 1;
  }
  std::cout << "[demo] resuming from " << ckpt_path << " (completed:";
  for (int i = 0; i < cp_or->completed_nodes_size(); ++i) {
    std::cout << " " << cp_or->completed_nodes(i);
  }
  std::cout << ")\n";

  af::Runner::Options opts;
  af::JsonlEventEmitter trace(std::cerr);
  opts.trace = &trace;
  af::Runner runner(BuildGraph(), opts);

  asio::io_context io;
  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<af::State> {
        co_return co_await runner.Resume(*cp_or,
                                          af::State::From(af::test::TestState{}));
      },
      asio::use_future);
  io.run();
  PrintFinal(fut.get());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string kill_after;
  std::string ckpt_path = kDefaultCkpt;
  bool resume = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "--kill-after" && i + 1 < argc) {
      kill_after = argv[++i];
    } else if (a == "--resume" && i + 1 < argc) {
      resume = true;
      ckpt_path = argv[++i];
    } else if (a == "--ckpt" && i + 1 < argc) {
      ckpt_path = argv[++i];
    } else {
      std::cerr << "usage: " << argv[0]
                << " [--kill-after NODE] [--resume PATH] [--ckpt PATH]\n";
      return 1;
    }
  }
  return resume ? RunResume(ckpt_path) : RunFresh(kill_after, ckpt_path);
}
