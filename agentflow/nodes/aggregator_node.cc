// agentflow/nodes/aggregator_node.cc
#include "agentflow/nodes/aggregator_node.h"

#include <utility>

#include "agentflow/core/errors.h"

namespace agentflow {

AggregatorNode::AggregatorNode(AggregatorNodeConfig cfg)
    : cfg_(std::move(cfg)) {
  if (cfg_.id.empty()) throw AgentflowError("AggregatorNode: id required");
}

asio::awaitable<State> AggregatorNode::Run(State state,
                                            const CancelToken& cancel,
                                            EventEmitter& emit) {
  if (cancel.IsCancelled()) co_return std::move(state);
  emit.EmitNodeStart(Id());
  if (cfg_.merger) {
    state = cfg_.merger(std::move(state));
  }
  emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);
  co_return std::move(state);
}

}  // namespace agentflow
