// agentflow/nodes/team_node.h
#ifndef AGENTFLOW_NODES_TEAM_NODE_H_
#define AGENTFLOW_NODES_TEAM_NODE_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <asio/awaitable.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/event.h"
#include "agentflow/core/node.h"
#include "agentflow/core/state.h"
#include "agentflow/nodes/llm_node.h"

namespace agentflow {

struct TeamNodeConfig {
  enum class Policy { StateRouter, ParallelGather, LlmSelect };
  Policy policy = Policy::StateRouter;

  std::string id;
  std::vector<std::unique_ptr<Node>> members;  // owned

  // Cap on the orchestration loop (LlmSelect / StateRouter). Mirrors
  // AgentNode::max_iter so users don't juggle two loop limits.
  int max_turns = 8;

  // ── StateRouter ───────────────────────────────────────────────────────────
  // Receives the current State; returns the next member's Id, or empty to
  // stop. Read-only access to the state under TeamNode's ownership.
  std::function<std::string(const State&)> router;

  // ── ParallelGather (T3) ───────────────────────────────────────────────────
  // Receives one State per member (same order as `members`). Returns the
  // merged State that the runner will fan out. If null, defaults to
  // last-writer-wins (matches Runner's P1 fan-in default).
  std::function<State(std::vector<State>)> aggregator;

  // ── LlmSelect (T4) ────────────────────────────────────────────────────────
  // Moderator LLM config. The moderator's input_field receives the per-turn
  // prompt naming the available members; its output_field is where we read
  // the chosen member's id back from.
  LlmNodeConfig moderator;
};

// Orchestrates a set of member Nodes by one of three policies (§5.3). TeamNode
// is itself a Node — outwardly a single graph vertex — but internally it
// co_awaits its members directly rather than recursing into Runner.
class TeamNode : public Node {
 public:
  explicit TeamNode(TeamNodeConfig cfg);
  ~TeamNode() override = default;

  std::string_view Id() const override { return cfg_.id; }
  std::string_view Kind() const override { return "team"; }

  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override;

 private:
  asio::awaitable<State> RunStateRouter(State state, const CancelToken& cancel,
                                         EventEmitter& emit);
  asio::awaitable<State> RunParallelGather(State state,
                                            const CancelToken& cancel,
                                            EventEmitter& emit);
  asio::awaitable<State> RunLlmSelect(State state, const CancelToken& cancel,
                                       EventEmitter& emit);

  Node* FindMember(std::string_view id) const;

  TeamNodeConfig cfg_;
};

}  // namespace agentflow
#endif  // AGENTFLOW_NODES_TEAM_NODE_H_
