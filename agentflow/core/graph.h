// agentflow/core/graph.h
#ifndef AGENTFLOW_CORE_GRAPH_H_
#define AGENTFLOW_CORE_GRAPH_H_

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agentflow/core/edge.h"
#include "agentflow/core/node.h"

namespace agentflow {

class GraphBuilder;

class Graph {
 public:
  const std::vector<Edge>& Edges() const { return edges_; }
  const std::vector<std::unique_ptr<Node>>& Nodes() const { return nodes_; }

  Node* FindNode(std::string_view id) const;

  // graphviz DOT dump, with activation_group + condition labels on each edge.
  std::string ToDotString() const;

  // Entry node IDs: nodes with no incoming activation_group=0 edge. The Runner
  // seeds these with the initial state. Cycle-only entry nodes are also valid
  // entries (they bootstrap via the Runner's times_fired==0 rule).
  const std::vector<std::string>& EntryNodeIds() const { return entry_ids_; }

 private:
  friend class GraphBuilder;
  Graph() = default;

  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<Edge> edges_;
  std::unordered_map<std::string, size_t> id_to_index_;
  std::vector<std::string> entry_ids_;
};

class GraphBuilder {
 public:
  GraphBuilder& AddNode(std::unique_ptr<Node> node);

  // group=0 by default — the runner treats group-0 incoming edges as standard
  // DAG fan-in (must drain to 0 before the node fires).
  GraphBuilder& AddEdge(std::string from, std::string to,
                        Edge::Condition cond = Edge::Condition::ALL);

  // user_group must be > 0 for cycle/back edges. The runner treats group>0
  // counters as "vacuously satisfied" on the node's first firing (cycle
  // bootstrap) and resets them on each subsequent firing.
  GraphBuilder& AddEdge(std::string from, std::string to,
                        int user_group, Edge::Condition cond);

  // Validates and returns immutable Graph. Throws GraphCompileError on failure.
  Graph Build();

 private:
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<Edge> edges_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_GRAPH_H_
