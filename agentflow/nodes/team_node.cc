// agentflow/nodes/team_node.cc
#include "agentflow/nodes/team_node.h"

#include <string>
#include <utility>

#include "agentflow/core/errors.h"

namespace agentflow {

TeamNode::TeamNode(TeamNodeConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.id.empty()) {
    throw AgentflowError("TeamNode: id required");
  }
  if (cfg_.members.empty()) {
    throw AgentflowError("TeamNode: at least one member required");
  }
}

Node* TeamNode::FindMember(std::string_view id) const {
  for (const auto& m : cfg_.members) {
    if (m->Id() == id) return m.get();
  }
  return nullptr;
}

asio::awaitable<State> TeamNode::Run(State state, const CancelToken& cancel,
                                      EventEmitter& emit) {
  switch (cfg_.policy) {
    case TeamNodeConfig::Policy::StateRouter:
      co_return co_await RunStateRouter(std::move(state), cancel, emit);
    case TeamNodeConfig::Policy::ParallelGather:
      co_return co_await RunParallelGather(std::move(state), cancel, emit);
    case TeamNodeConfig::Policy::LlmSelect:
      co_return co_await RunLlmSelect(std::move(state), cancel, emit);
  }
  throw AgentflowError("TeamNode: unknown policy");
}

asio::awaitable<State> TeamNode::RunStateRouter(State state,
                                                 const CancelToken& cancel,
                                                 EventEmitter& emit) {
  if (!cfg_.router) {
    throw AgentflowError("TeamNode StateRouter: router fn required");
  }
  for (int turn = 0; turn < cfg_.max_turns; ++turn) {
    if (cancel.IsCancelled()) co_return std::move(state);

    std::string next = cfg_.router(state);
    if (next.empty()) co_return std::move(state);  // router signalled "done"

    Node* member = FindMember(next);
    if (!member) {
      throw AgentflowError(
          "TeamNode: router returned unknown member id: " + next);
    }
    emit.EmitNodeStart(Id());
    state = co_await member->Run(std::move(state), cancel, emit);
    emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);
  }
  co_return std::move(state);  // max_turns reached — return whatever we have
}

asio::awaitable<State> TeamNode::RunParallelGather(
    State /*state*/, const CancelToken& /*cancel*/, EventEmitter& /*emit*/) {
  throw AgentflowError("TeamNode ParallelGather: not implemented (T3)");
  co_return State{};  // unreachable; satisfies coroutine return contract
}

asio::awaitable<State> TeamNode::RunLlmSelect(
    State /*state*/, const CancelToken& /*cancel*/, EventEmitter& /*emit*/) {
  throw AgentflowError("TeamNode LlmSelect: not implemented (T4)");
  co_return State{};
}

}  // namespace agentflow
