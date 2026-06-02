// agentflow/nodes/router_node.h
#ifndef AGENTFLOW_NODES_ROUTER_NODE_H_
#define AGENTFLOW_NODES_ROUTER_NODE_H_

#include <functional>
#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/node.h"
#include "agentflow/core/state.h"

namespace agentflow {

// Reads State, returns the NodeId of the downstream this graph should route
// to. The decision is written into a configurable string field on the output
// State; downstream nodes consult that field via their own logic (RouterNode
// itself doesn't perturb the runner's edge mechanics — the existing
// activation_group + per-edge condition still handles the structural side).
struct RouterNodeConfig {
  std::string id;
  std::function<std::string(const State&)> chooser;
  std::string output_field;  // string field to write the chosen NodeId into
};

class RouterNode : public Node {
 public:
  explicit RouterNode(RouterNodeConfig cfg);
  ~RouterNode() override = default;

  std::string_view Id() const override { return cfg_.id; }
  std::string_view Kind() const override { return "router"; }

  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;

 private:
  RouterNodeConfig cfg_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_NODES_ROUTER_NODE_H_
