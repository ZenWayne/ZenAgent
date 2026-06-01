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
#include "agentflow/nodes/llm_node.h"

#include <algorithm>
#include <cctype>

namespace agentflow {

namespace {

// Mirror of LlmNode/AgentNode's reflection helpers. (Lift into
// state_field_util.h when a fourth node needs them.)
std::string ReadStringField(const State& s, const std::string& f) {
  if (f.empty()) return {};
  const auto* msg = s.UnsafeMessage();
  if (!msg) return {};
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(f);
  if (!desc) return {};
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    return refl->GetString(*msg, desc);
  }
  return {};
}

void WriteStringField(State& s, const std::string& f, const std::string& v) {
  if (f.empty()) return;
  auto* msg = const_cast<google::protobuf::Message*>(s.UnsafeMessage());
  if (!msg) return;
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(f);
  if (!desc) return;
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    refl->SetString(msg, desc, v);
  }
}

std::string TrimAndLower(std::string_view s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

std::string Trim(std::string_view s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return std::string(s);
}

}  // namespace

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

asio::awaitable<State> TeamNode::RunLlmSelect(State state,
                                               const CancelToken& cancel,
                                               EventEmitter& emit) {
  // Build the once-per-team listing of "id (kind)" lines for the moderator
  // prompt. We rebuild it each turn (cheap) so a future hot-swap of members
  // would Just Work.
  auto build_listing = [this]() {
    std::string out;
    for (const auto& m : cfg_.members) {
      out.append("- ");
      out.append(m->Id());
      out.append(" (");
      out.append(m->Kind());
      out.append(")\n");
    }
    return out;
  };

  LlmNode moderator(cfg_.moderator);
  const std::string& mod_in = cfg_.moderator.input_field;
  const std::string& mod_out = cfg_.moderator.output_field;

  for (int turn = 0; turn < cfg_.max_turns; ++turn) {
    if (cancel.IsCancelled()) co_return std::move(state);

    // Write the per-turn prompt into the moderator's input_field on a CLONE
    // of the live state. We don't mutate the live state with the prompt —
    // the moderator's view is throwaway; only its decision matters.
    State mod_state = state.Clone();
    std::string prompt =
        "Members available:\n" + build_listing() +
        "Reply with exactly one member id from the list above, or DONE to "
        "stop.";
    WriteStringField(mod_state, mod_in, prompt);

    mod_state = co_await moderator.Run(std::move(mod_state), cancel, emit);
    if (cancel.IsCancelled()) co_return std::move(state);

    std::string decision = Trim(ReadStringField(mod_state, mod_out));
    if (decision.empty() || TrimAndLower(decision) == "done") {
      co_return std::move(state);
    }

    Node* member = FindMember(decision);
    if (!member) {
      // Moderator hallucinated. Record a note on the LIVE state so the next
      // turn's moderator prompt (which the user's system_prompt can pull
      // into context) can see what went wrong, then continue.
      std::string note = ReadStringField(state, mod_in);
      if (!note.empty()) note.push_back('\n');
      note.append("[moderator-error] unknown member: ");
      note.append(decision);
      WriteStringField(state, mod_in, note);
      continue;
    }

    emit.EmitNodeStart(Id());
    state = co_await member->Run(std::move(state), cancel, emit);
    emit.EmitNodeEnd(Id(), cancel.IsCancelled(), /*failed=*/false);
  }
  co_return std::move(state);  // max_turns reached
}

}  // namespace agentflow
