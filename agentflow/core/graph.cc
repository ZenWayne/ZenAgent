// agentflow/core/graph.cc
#include "agentflow/core/graph.h"

#include <sstream>
#include <unordered_set>

#include "agentflow/core/errors.h"

namespace agentflow {

GraphBuilder& GraphBuilder::AddNode(std::unique_ptr<Node> node) {
  nodes_.push_back(std::move(node));
  return *this;
}

GraphBuilder& GraphBuilder::AddEdge(std::string from, std::string to,
                                    Edge::Condition cond) {
  edges_.push_back(Edge{std::move(from), std::move(to), 0, cond});
  return *this;
}

GraphBuilder& GraphBuilder::AddEdge(std::string from, std::string to,
                                    int user_group, Edge::Condition cond) {
  if (user_group <= 0) {
    throw GraphCompileError("user_group must be > 0 (use 0-arg overload "
                            "for non-cycle edges)");
  }
  edges_.push_back(Edge{std::move(from), std::move(to), user_group, cond});
  return *this;
}

Graph GraphBuilder::Build() {
  Graph g;
  g.nodes_ = std::move(nodes_);

  std::vector<std::string> ids;
  ids.reserve(g.nodes_.size());
  for (size_t i = 0; i < g.nodes_.size(); ++i) {
    const auto& id = g.nodes_[i]->Id();
    if (g.id_to_index_.count(id)) {
      throw GraphCompileError("duplicate node id: " + id);
    }
    g.id_to_index_[id] = i;
    ids.push_back(id);
  }

  g.edges_ = std::move(edges_);

  // Endpoint sanity check.
  for (const auto& e : g.edges_) {
    if (!g.id_to_index_.count(e.from) || !g.id_to_index_.count(e.to)) {
      throw GraphCompileError("edge references unknown node: " + e.from +
                              " -> " + e.to);
    }
  }

  // Per-node-per-group condition consistency.
  std::unordered_map<std::string, std::unordered_map<int, Edge::Condition>>
      cond_by_node_group;
  for (const auto& e : g.edges_) {
    auto& m = cond_by_node_group[e.to];
    auto it = m.find(e.activation_group);
    if (it == m.end()) {
      m[e.activation_group] = e.condition;
    } else if (it->second != e.condition) {
      throw GraphCompileError(
          "node '" + e.to + "' has conflicting Condition (ALL vs ANY) "
          "for activation_group " + std::to_string(e.activation_group));
    }
  }

  // Entry nodes: those with no incoming group=0 edge. Cycle-only-incoming
  // nodes also count as entries — they bootstrap via the Runner's
  // times_fired==0 rule.
  std::unordered_set<std::string> has_g0_in;
  for (const auto& e : g.edges_) {
    if (e.activation_group == 0) has_g0_in.insert(e.to);
  }
  for (const auto& id : ids) {
    if (!has_g0_in.count(id)) g.entry_ids_.push_back(id);
  }
  if (g.entry_ids_.empty()) {
    throw GraphCompileError("graph has no entry node (every node has a "
                            "group=0 incoming edge)");
  }

  return g;
}

Node* Graph::FindNode(std::string_view id) const {
  auto it = id_to_index_.find(std::string(id));
  if (it == id_to_index_.end()) return nullptr;
  return nodes_[it->second].get();
}

std::string Graph::ToDotString() const {
  std::ostringstream os;
  os << "digraph G {\n";
  for (const auto& n : nodes_) {
    os << "  \"" << n->Id() << "\" [label=\"" << n->Id()
       << "\\n(" << n->Kind() << ")\"];\n";
  }
  for (const auto& e : edges_) {
    os << "  \"" << e.from << "\" -> \"" << e.to << "\" [label=\"g="
       << e.activation_group
       << " " << (e.condition == Edge::Condition::ALL ? "ALL" : "ANY")
       << "\"];\n";
  }
  os << "}\n";
  return os.str();
}

}  // namespace agentflow
