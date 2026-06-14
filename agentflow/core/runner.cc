// agentflow/core/runner.cc
#include "agentflow/core/runner.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/experimental/channel.hpp>

#include "agentflow/core/errors.h"
#include "agentflow/persist/checkpoint_writer.h"
#include "checkpoint.pb.h"

namespace agentflow {

namespace {

using Channel = asio::experimental::channel<void(asio::error_code, int)>;

// Per-node activation bookkeeping, keyed by activation_group.
struct NodeActivation {
  // group_id -> remaining ALL count (decremented as each edge fires).
  std::unordered_map<int, int> remaining_all;
  // group_id -> condition (ALL or ANY).
  std::unordered_map<int, Edge::Condition> conditions;
  // group_id -> at least one edge fired (ANY tracking).
  std::unordered_map<int, bool> any_fired;
  // Inputs accumulated since the last firing.
  std::vector<State> pending_inputs;
  // Number of times this node has fired so far. Used for cycle bootstrap:
  // groups with id>0 are vacuously satisfied while times_fired == 0.
  int times_fired = 0;
  // Reentrancy guard: true while a coroutine for this node is in flight.
  bool in_flight = false;
};

NullEventEmitter& NullEmitterSingleton() {
  static NullEventEmitter inst;
  return inst;
}

}  // namespace

class Runner::Impl {
 public:
  Impl(Graph g, Options opts)
      : graph_(std::move(g)),
        opts_(opts),
        emit_(opts.trace ? *opts.trace : NullEmitterSingleton()) {
    if (opts_.checkpoint_policy != CheckpointPolicy::Off &&
        opts_.checkpoint_writer == nullptr) {
      throw AgentflowError(
          "Runner: checkpoint_writer required when policy != Off");
    }
  }

  asio::awaitable<State> Run(State initial, CancelToken cancel);
  asio::awaitable<State> Resume(const proto::Checkpoint& cp, State target,
                                 CancelToken cancel);

 private:
  void InitActivations();
  bool NodeReady(const NodeActivation& na) const;
  static State MergeInputs(std::vector<State> inputs);
  void MaybeWriteCheckpoint(const State& current_state, bool terminal);
  asio::awaitable<State> RunInner(CancelToken cancel);

  Graph graph_;
  Options opts_;
  EventEmitter& emit_;
  std::mutex mu_;
  std::unordered_map<std::string, NodeActivation> activations_;
  // Baseline per-node initial-count maps captured at Run() start; used to
  // reset cycle (group>0) counters at fire-decision time. We only snapshot
  // the counter map here because NodeActivation itself is non-copyable.
  std::unordered_map<std::string, std::unordered_map<int, int>> baseline_;
  std::optional<State> terminal_state_;
  std::atomic<int> in_flight_count_{0};
  std::exception_ptr first_error_;
  // Ordered list of node ids that completed successfully (NODE_END,
  // !failed, !cancelled). Persisted into Checkpoint.completed_nodes.
  std::vector<std::string> completed_node_ids_;
};

void Runner::Impl::MaybeWriteCheckpoint(const State& current_state,
                                         bool terminal) {
  if (opts_.checkpoint_policy == CheckpointPolicy::Off) return;
  if (opts_.checkpoint_policy == CheckpointPolicy::AfterTerminalNode &&
      !terminal) {
    return;
  }
  proto::Checkpoint cp;
  cp.set_state_bytes(current_state.SerializeAsString());
  if (current_state.UnsafeMessage() != nullptr) {
    cp.set_state_type(
        std::string(current_state.UnsafeMessage()->GetTypeName()));
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& id : completed_node_ids_) cp.add_completed_nodes(id);
  }
  cp.set_unix_micros(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  cp.set_schema_version(kCurrentCheckpointSchemaVersion);
  // Write errors are non-fatal: log and continue. Run() shouldn't crash
  // because a disk got full or someone yanked a USB stick.
  (void)opts_.checkpoint_writer->Write(cp);
}

void Runner::Impl::InitActivations() {
  for (const auto& np : graph_.Nodes()) {
    activations_.try_emplace(std::string(np->Id()));
  }
  for (const auto& e : graph_.Edges()) {
    auto& na = activations_[e.to];
    if (!na.conditions.contains(e.activation_group)) {
      na.conditions[e.activation_group] = e.condition;
    }
    na.remaining_all[e.activation_group] += 1;
    na.any_fired.try_emplace(e.activation_group, false);
  }
  // Snapshot only the counter maps (NodeActivation is non-copyable because
  // pending_inputs holds non-copyable State values; we only need the per-group
  // counts here for cycle resets).
  baseline_.clear();
  for (const auto& [id, na] : activations_) {
    baseline_[id] = na.remaining_all;
  }
}

bool Runner::Impl::NodeReady(const NodeActivation& na) const {
  for (const auto& [g, cond] : na.conditions) {
    // Cycle bootstrap: groups with id > 0 are vacuously satisfied until the
    // node has fired once.
    if (g > 0 && na.times_fired == 0) continue;

    if (cond == Edge::Condition::ALL) {
      auto it = na.remaining_all.find(g);
      int rem = (it == na.remaining_all.end()) ? 0 : it->second;
      if (rem > 0) return false;
    } else {  // ANY
      auto it = na.any_fired.find(g);
      if (it == na.any_fired.end() || !it->second) return false;
    }
  }
  return true;
}

State Runner::Impl::MergeInputs(std::vector<State> inputs) {
  if (inputs.empty()) return State{};
  return std::move(inputs.back());
}

asio::awaitable<State> Runner::Impl::Run(State initial, CancelToken cancel) {
  InitActivations();

  auto exec = co_await asio::this_coro::executor;
  Channel done(exec, /*max_buffer=*/1024);

  // launch() must do everything that affects readiness *before* spawning the
  // coroutine, so the synchronous fire-decision state is stable.
  auto launch = [&](const std::string& node_id) {
    State input;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& na = activations_[node_id];
      input = MergeInputs(std::move(na.pending_inputs));
      na.pending_inputs.clear();
      na.in_flight = true;
      na.times_fired += 1;
      // Reset cycle-group counters NOW so predecessor decrements that arrive
      // during execution accumulate against the fresh baseline. Group 0 stays
      // drained.
      for (auto& [g, rem] : na.remaining_all) {
        if (g == 0) continue;
        rem = baseline_[node_id][g];
      }
      for (auto& [g, fired] : na.any_fired) {
        if (g == 0) continue;
        fired = false;
      }
    }
    in_flight_count_.fetch_add(1);
    Node* node = graph_.FindNode(node_id);

    asio::co_spawn(exec,
      [this, node, node_id, &done, &cancel,
       input = std::move(input)]() mutable -> asio::awaitable<void> {
        const std::string_view node_id_view = node_id;
        State out;
        bool failed = false;
        emit_.EmitNodeStart(node_id_view);
        try {
          out = co_await node->Run(std::move(input), cancel, emit_);
        } catch (...) {
          {
            std::lock_guard<std::mutex> lk(mu_);
            if (!first_error_) first_error_ = std::current_exception();
          }
          emit_.EmitNodeFailed(node_id_view, "Exception", "node threw");
          failed = true;
        }
        emit_.EmitNodeEnd(node_id_view, /*cancelled=*/cancel.IsCancelled(),
                          /*failed=*/failed);

        // Track completion (successful + not cancelled) for the checkpoint
        // log. Failed/cancelled nodes are NOT recorded — Resume must not
        // skip a node that didn't actually produce output.
        if (!failed && !cancel.IsCancelled()) {
          std::lock_guard<std::mutex> lk(mu_);
          completed_node_ids_.push_back(std::string(node_id_view));
        }

        // Cancellation skips fan-out: a cancelled node returns normally
        // (per Node contract — exit promptly without throwing) but its output
        // is partial/undefined. Treating it like the !failed path would push
        // stale state to successors; treating it like failed would propagate
        // an error. We do neither — quiesce silently and let the caller see
        // an empty terminal state.
        if (!failed && !cancel.IsCancelled()) {
          bool has_outgoing = false;
          for (const auto& e : graph_.Edges()) {
            if (e.from != node_id_view) continue;
            has_outgoing = true;
            std::lock_guard<std::mutex> lk(mu_);
            auto& target = activations_[e.to];
            target.pending_inputs.push_back(out.Clone());
            if (e.condition == Edge::Condition::ALL) {
              target.remaining_all[e.activation_group] -= 1;
            } else {
              target.any_fired[e.activation_group] = true;
            }
            emit_.EmitEdgeFire(e.from, e.to, e.activation_group);
          }
          if (!has_outgoing) {
            std::lock_guard<std::mutex> lk(mu_);
            terminal_state_ = out.Clone();
          }
        }

        // Checkpoint hook — fires after fan-out so cp.completed_nodes
        // includes this node, and the state snapshot is what this node
        // produced. Skipped on failure/cancel.
        if (!failed && !cancel.IsCancelled()) {
          const bool is_terminal = [&] {
            for (const auto& e : graph_.Edges()) {
              if (e.from == node_id_view) return false;
            }
            return true;
          }();
          MaybeWriteCheckpoint(out, is_terminal);
        }

        {
          std::lock_guard<std::mutex> lk(mu_);
          activations_[node_id].in_flight = false;
        }
        in_flight_count_.fetch_sub(1);
        co_await done.async_send(asio::error_code{}, 1, asio::use_awaitable);
        co_return;
      },
      asio::detached);
  };

  // Bootstrap: deliver the initial state to entry nodes, then dispatch any
  // that are ready.
  std::vector<std::string> ready;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& id : graph_.EntryNodeIds()) {
      activations_[id].pending_inputs.push_back(initial.Clone());
    }
    for (const auto& id : graph_.EntryNodeIds()) {
      if (NodeReady(activations_[id])) ready.push_back(id);
    }
  }
  for (const auto& id : ready) launch(id);

  // Main loop. With a single-threaded executor, asio::co_spawn may run a fast
  // coroutine fully inline, so a launch can synchronously queue more pending
  // inputs. The inner drain loop dispatches everything ready right now;
  // the outer loop then waits on the done channel for any async-completing
  // nodes to signal progress.
  while (true) {
    while (true) {
      // Invariant: each node_id appears at most once in activations_, and the
      // !in_flight check below runs under mu_ — so `next` cannot contain
      // duplicates and we don't need explicit dedup before launch().
      std::vector<std::string> next;
      {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, na] : activations_) {
          if (na.in_flight) continue;
          if (na.pending_inputs.empty()) continue;
          if (NodeReady(na)) next.push_back(id);
        }
      }
      if (next.empty()) break;
      for (const auto& id : next) launch(id);
    }

    if (in_flight_count_.load() == 0) break;

    co_await done.async_receive(asio::use_awaitable);

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (first_error_ && in_flight_count_.load() == 0) {
        emit_.EmitGraphDone(/*failed=*/true);
        std::rethrow_exception(first_error_);
      }
      if (first_error_) continue;
    }
  }

  if (first_error_) {
    emit_.EmitGraphDone(/*failed=*/true);
    std::rethrow_exception(first_error_);
  }

  emit_.EmitGraphDone(/*failed=*/false);
  if (terminal_state_) co_return std::move(*terminal_state_);
  co_return State{};
}

asio::awaitable<State> Runner::Impl::Resume(const proto::Checkpoint& cp,
                                              State target,
                                              CancelToken cancel) {
  if (!target.UnsafeMessage()) {
    throw AgentflowError(
        "Runner::Resume: target State has no underlying message; "
        "construct via State::From<YourProto>{}");
  }
  if (!cp.state_type().empty() &&
      cp.state_type() != target.UnsafeMessage()->GetTypeName()) {
    throw AgentflowError(
        "Runner::Resume: checkpoint state_type='" + cp.state_type() +
        "' does not match target='" +
        std::string(target.UnsafeMessage()->GetTypeName()) + "'");
  }
  if (auto st = target.ParseFromStringBounded(cp.state_bytes()); !st.ok()) {
    throw AgentflowError("Runner::Resume: " + std::string(st.message()));
  }

  InitActivations();

  // Build a set of completed ids for fast lookup; verify each exists.
  std::unordered_set<std::string> completed;
  for (const auto& id : cp.completed_nodes()) {
    if (graph_.FindNode(id) == nullptr) {
      throw AgentflowError("Runner::Resume: unknown node id in checkpoint: " +
                            id);
    }
    completed.insert(id);
  }

  {
    std::lock_guard<std::mutex> lk(mu_);

    if (cp.completed_nodes().empty()) {
      // Cold-start: no prior progress recorded. Seed entry nodes with
      // `target` exactly as Run() does, and let RunInner dispatch normally.
      for (const auto& id : graph_.EntryNodeIds()) {
        activations_[id].pending_inputs.push_back(target.Clone());
      }
    } else {
      // Replay completion bookkeeping: each completed node "fired once,"
      // counted toward our completed_node_ids_ log.
      for (const auto& id : cp.completed_nodes()) {
        auto& na = activations_[id];
        na.times_fired = 1;
        na.in_flight = false;
        completed_node_ids_.push_back(id);
      }
      // Seed successors of every completed node — but skip edges whose target
      // is itself already completed (that input was consumed in the prior run).
      bool any_outgoing_alive = false;
      for (const auto& id : cp.completed_nodes()) {
        bool has_outgoing = false;
        for (const auto& e : graph_.Edges()) {
          if (e.from != id) continue;
          has_outgoing = true;
          if (completed.count(e.to)) continue;
          auto& succ = activations_[e.to];
          succ.pending_inputs.push_back(target.Clone());
          if (e.condition == Edge::Condition::ALL) {
            succ.remaining_all[e.activation_group] -= 1;
          } else {
            succ.any_fired[e.activation_group] = true;
          }
        }
        if (has_outgoing) any_outgoing_alive = true;
      }
      // If every completed node was terminal (no outgoing) or the graph has
      // nothing left to do, take target as the terminal state immediately.
      if (!any_outgoing_alive ||
          completed.size() == graph_.Nodes().size()) {
        terminal_state_ = target.Clone();
      }
    }
  }

  return RunInner(std::move(cancel));
}

asio::awaitable<State> Runner::Impl::RunInner(CancelToken cancel) {
  auto exec = co_await asio::this_coro::executor;
  Channel done(exec, /*max_buffer=*/1024);

  auto launch = [&](const std::string& node_id) {
    State input;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& na = activations_[node_id];
      input = MergeInputs(std::move(na.pending_inputs));
      na.pending_inputs.clear();
      na.in_flight = true;
      na.times_fired += 1;
      for (auto& [g, rem] : na.remaining_all) {
        if (g == 0) continue;
        rem = baseline_[node_id][g];
      }
      for (auto& [g, fired] : na.any_fired) {
        if (g == 0) continue;
        fired = false;
      }
    }
    in_flight_count_.fetch_add(1);
    Node* node = graph_.FindNode(node_id);

    asio::co_spawn(exec,
      [this, node, node_id, &done, &cancel,
       input = std::move(input)]() mutable -> asio::awaitable<void> {
        const std::string_view node_id_view = node_id;
        State out;
        bool failed = false;
        emit_.EmitNodeStart(node_id_view);
        try {
          out = co_await node->Run(std::move(input), cancel, emit_);
        } catch (...) {
          {
            std::lock_guard<std::mutex> lk(mu_);
            if (!first_error_) first_error_ = std::current_exception();
          }
          emit_.EmitNodeFailed(node_id_view, "Exception", "node threw");
          failed = true;
        }
        emit_.EmitNodeEnd(node_id_view, cancel.IsCancelled(), failed);

        if (!failed && !cancel.IsCancelled()) {
          std::lock_guard<std::mutex> lk(mu_);
          completed_node_ids_.push_back(std::string(node_id_view));
        }

        if (!failed && !cancel.IsCancelled()) {
          bool has_outgoing = false;
          for (const auto& e : graph_.Edges()) {
            if (e.from != node_id_view) continue;
            has_outgoing = true;
            std::lock_guard<std::mutex> lk(mu_);
            auto& target = activations_[e.to];
            target.pending_inputs.push_back(out.Clone());
            if (e.condition == Edge::Condition::ALL) {
              target.remaining_all[e.activation_group] -= 1;
            } else {
              target.any_fired[e.activation_group] = true;
            }
            emit_.EmitEdgeFire(e.from, e.to, e.activation_group);
          }
          if (!has_outgoing) {
            std::lock_guard<std::mutex> lk(mu_);
            terminal_state_ = out.Clone();
          }

          const bool is_terminal = !has_outgoing;
          MaybeWriteCheckpoint(out, is_terminal);
        }

        {
          std::lock_guard<std::mutex> lk(mu_);
          activations_[node_id].in_flight = false;
        }
        in_flight_count_.fetch_sub(1);
        co_await done.async_send(asio::error_code{}, 1, asio::use_awaitable);
        co_return;
      },
      asio::detached);
  };

  // Initial dispatch: anything already ready (Resume may have seeded some).
  std::vector<std::string> ready;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, na] : activations_) {
      if (na.in_flight) continue;
      if (na.pending_inputs.empty()) continue;
      if (NodeReady(na)) ready.push_back(id);
    }
  }
  for (const auto& id : ready) launch(id);

  while (true) {
    while (true) {
      std::vector<std::string> next;
      {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, na] : activations_) {
          if (na.in_flight) continue;
          if (na.pending_inputs.empty()) continue;
          if (NodeReady(na)) next.push_back(id);
        }
      }
      if (next.empty()) break;
      for (const auto& id : next) launch(id);
    }

    if (in_flight_count_.load() == 0) break;
    co_await done.async_receive(asio::use_awaitable);
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (first_error_ && in_flight_count_.load() == 0) {
        emit_.EmitGraphDone(true);
        std::rethrow_exception(first_error_);
      }
      if (first_error_) continue;
    }
  }

  if (first_error_) {
    emit_.EmitGraphDone(true);
    std::rethrow_exception(first_error_);
  }
  emit_.EmitGraphDone(false);
  if (terminal_state_) co_return std::move(*terminal_state_);
  co_return State{};
}

Runner::Runner(Graph g, Options opts)
    : impl_(std::make_unique<Impl>(std::move(g), opts)) {}
Runner::~Runner() = default;

asio::awaitable<State> Runner::Run(State initial, CancelToken cancel) {
  return impl_->Run(std::move(initial), std::move(cancel));
}

asio::awaitable<State> Runner::Resume(const proto::Checkpoint& cp,
                                       State target, CancelToken cancel) {
  return impl_->Resume(cp, std::move(target), std::move(cancel));
}

}  // namespace agentflow
