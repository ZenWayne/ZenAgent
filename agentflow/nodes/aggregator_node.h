// agentflow/nodes/aggregator_node.h
#ifndef AGENTFLOW_NODES_AGGREGATOR_NODE_H_
#define AGENTFLOW_NODES_AGGREGATOR_NODE_H_

#include <functional>
#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/node.h"
#include "agentflow/core/state.h"

namespace agentflow {

// Fan-in aggregation point. By the time AggregatorNode::Run is called the
// runner has already merged its upstream inputs into a single State per the
// active fan-in policy (default last-writer-wins, per P1). The user-supplied
// `merger` then gets to do anything more sophisticated — e.g., majority-vote
// a string field, concatenate token streams, score-and-pick.
struct AggregatorNodeConfig {
  std::string id;
  // Receives the merged State; returns the post-aggregation State.
  // If null, AggregatorNode is an identity pass-through.
  std::function<State(State)> merger;
};

class AggregatorNode : public Node {
 public:
  explicit AggregatorNode(AggregatorNodeConfig cfg);
  ~AggregatorNode() override = default;

  std::string_view Id() const override { return cfg_.id; }
  std::string_view Kind() const override { return "aggregator"; }

  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;

 private:
  AggregatorNodeConfig cfg_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_NODES_AGGREGATOR_NODE_H_
