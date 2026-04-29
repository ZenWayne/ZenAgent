// agentflow/core/node.h
#ifndef AGENTFLOW_CORE_NODE_H_
#define AGENTFLOW_CORE_NODE_H_

#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/state.h"

namespace agentflow {

class Node {
 public:
  using NodeId = std::string;

  virtual ~Node() = default;

  virtual NodeId Id() const = 0;
  virtual std::string_view Kind() const = 0;

  // The runner gives the node an exclusive State view; the node returns a new
  // State (move) as the output that the runner will fan out to downstream
  // edges.
  //
  // Contract:
  // - The node MUST observe `cancel` and exit promptly when triggered. LLM
  //   nodes (later plans) wire `cancel.OnCancel(...)` to engine-level abort.
  // - Throwing from Run() = node failure (runner emits NodeFailed and applies
  //   the failure policy).
  // - The node owns its execution context for the duration of Run.
  virtual asio::awaitable<State> Run(State state,
                                      const CancelToken& cancel,
                                      EventEmitter& emit) = 0;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_NODE_H_
