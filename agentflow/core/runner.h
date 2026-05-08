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

// Runs a compiled Graph to completion. P1 keeps things simple:
//  - Fan-in merge: last-writer-wins.
//  - Failure: any node throwing aborts the whole graph.
//  - Streaming: events are emitted via EventEmitter (passed in Options).
class Runner {
 public:
  struct Options {
    // Non-owning. Must outlive Run(). If null, a process-wide NullEventEmitter
    // is used and trace events are dropped.
    EventEmitter* trace = nullptr;
  };

  Runner(Graph graph, Options opts);
  ~Runner();

  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  // Returns the State after the last terminal node finishes. Throws on
  // any node failure (the wrapped exception propagates).
  asio::awaitable<State> Run(State initial, CancelToken cancel = CancelToken());

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_RUNNER_H_
