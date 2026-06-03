// agentflow/core/runner.h
#ifndef AGENTFLOW_CORE_RUNNER_H_
#define AGENTFLOW_CORE_RUNNER_H_

#include <memory>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/graph.h"
#include "agentflow/core/state.h"

namespace agentflow {

class CheckpointWriter;  // agentflow/persist/checkpoint_writer.h
namespace proto { class Checkpoint; }  // proto/checkpoint.proto

// Runs a compiled Graph to completion. P1 keeps things simple:
//  - Fan-in merge: last-writer-wins.
//  - Failure: any node throwing aborts the whole graph.
//  - Streaming: events are emitted via EventEmitter (passed in Options).
class Runner {
 public:
  enum class CheckpointPolicy {
    Off,                  // never write checkpoints
    AfterEachNode,        // write after every NODE_END (high fidelity, more I/O)
    AfterTerminalNode,    // write only when the graph reaches a terminal node
  };

  struct Options {
    // Non-owning. Must outlive Run(). If null, a process-wide NullEventEmitter
    // is used and trace events are dropped.
    EventEmitter* trace = nullptr;

    CheckpointPolicy checkpoint_policy = CheckpointPolicy::Off;
    // Non-owning. Must outlive Run() and be non-null when policy != Off.
    CheckpointWriter* checkpoint_writer = nullptr;
  };

  Runner(Graph graph, Options opts);
  ~Runner();

  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  // Returns the State after the last terminal node finishes. Throws on
  // any node failure (the wrapped exception propagates).
  asio::awaitable<State> Run(State initial, CancelToken cancel = CancelToken());

  // Manual resume from a checkpoint. `target` is an empty State of the
  // correct concrete protobuf type (i.e. State::From<UserStateType>{}).
  // We need it because the Runner doesn't know the user's proto type — only
  // the caller does. Resume populates `target` from cp.state_bytes(), marks
  // every node in cp.completed_nodes() as done, seeds those nodes' successors
  // (that aren't themselves in completed_nodes) with the loaded state, and
  // continues normal scheduling.
  //
  // Edge cases:
  //   - cp.completed_nodes() empty == cold start with `target` as initial.
  //   - all nodes completed == returns target unchanged.
  //   - cp.state_type() must match target's message type, else AgentflowError.
  //   - any unknown node id in cp.completed_nodes() throws AgentflowError.
  asio::awaitable<State> Resume(const proto::Checkpoint& cp, State target,
                                 CancelToken cancel = CancelToken());

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_RUNNER_H_
