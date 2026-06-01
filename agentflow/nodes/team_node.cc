// agentflow/nodes/team_node.cc
#include "agentflow/nodes/team_node.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

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

asio::awaitable<State> TeamNode::RunParallelGather(State state,
                                                    const CancelToken& cancel,
                                                    EventEmitter& emit) {
  emit.EmitNodeStart(Id());

  using ResultChannel =
      asio::experimental::channel<void(asio::error_code, State)>;
  auto exec = co_await asio::this_coro::executor;

  // Spawn one branch per member. Each branch:
  //   (a) clones the entry state so concurrent Run()s don't race on a shared
  //       protobuf;
  //   (b) catches member exceptions so one bad member doesn't break gather —
  //       the channel is closed in that case and the result is skipped.
  std::vector<std::shared_ptr<ResultChannel>> channels;
  channels.reserve(cfg_.members.size());
  for (auto& member_up : cfg_.members) {
    auto ch = std::make_shared<ResultChannel>(exec, 1);
    channels.push_back(ch);
    Node* member = member_up.get();
    State input = state.Clone();
    asio::co_spawn(
        exec,
        [member, input = std::move(input), &cancel, &emit,
         ch]() mutable -> asio::awaitable<void> {
          try {
            State out =
                co_await member->Run(std::move(input), cancel, emit);
            asio::error_code ec;
            ch->try_send(ec, std::move(out));
          } catch (...) {
            ch->close();
          }
        },
        asio::detached);
  }

  std::vector<State> outs;
  outs.reserve(channels.size());
  for (auto& ch : channels) {
    auto [ec, s] =
        co_await ch->async_receive(asio::as_tuple(asio::use_awaitable));
    if (!ec) outs.push_back(std::move(s));
    // ec set (channel closed) ⇒ member threw; drop silently.
  }

  emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);

  if (cfg_.aggregator) {
    co_return cfg_.aggregator(std::move(outs));
  }
  // Default = last-writer-wins (matches Runner's P1 fan-in default).
  if (outs.empty()) co_return std::move(state);
  co_return std::move(outs.back());
}

asio::awaitable<State> TeamNode::RunLlmSelect(
    State /*state*/, const CancelToken& /*cancel*/, EventEmitter& /*emit*/) {
  throw AgentflowError("TeamNode LlmSelect: not implemented (T4)");
  co_return State{};
}

}  // namespace agentflow
