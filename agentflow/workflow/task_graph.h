#ifndef AGENTFLOW_WORKFLOW_TASK_GRAPH_H_
#define AGENTFLOW_WORKFLOW_TASK_GRAPH_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace agentflow::workflow {

// One planned unit of delegated work. The graph treats every field as data:
// `goal` is forwarded verbatim to the child agent, `agent` must be one of the
// parent's delegate targets and `fork` follows the existing delegate fork
// contract.
struct TaskNode {
  std::string id;
  std::string agent;
  std::string goal;
  std::vector<std::string> depends_on;
  bool fork = false;
};

// Result of one execution attempt. `outcome` keeps the child's own verdict so
// the parent can report it verbatim: "completed", "failed" or "blocked".
struct TaskResult {
  std::string summary;
  std::string outcome;
};

// Stored lifecycle state. `ready` and `blocked` are derived from dependencies
// and are never stored; see TaskStatus.
enum class TaskState { kPending, kRunning, kCompleted, kFailed, kCancelled };

// Effective status: the stored TaskState with `pending` refined by the state of
// its dependencies (pending + all dependencies completed == ready, otherwise
// blocked).
enum class TaskStatus {
  kReady,
  kBlocked,
  kRunning,
  kCompleted,
  kFailed,
  kCancelled,
};

// Stable lowercase names for logging and tool payloads.
std::string_view TaskStateName(TaskState state);
std::string_view TaskStatusName(TaskStatus status);

struct TaskGraphLimits {
  int max_tasks = 24;
  // Task ids are [a-z0-9-]{1,max_id_length}; the hard ceiling is 32.
  int max_id_length = 32;
  // Goals are limited in UTF-8 code points, not bytes, matching the plan's
  // "≤8000 characters".
  std::size_t max_goal_chars = 8000;
};

// Immutable snapshot of one node plus its current status and, once recorded,
// its result.
struct TaskNodeView {
  TaskNode node;
  TaskStatus status = TaskStatus::kBlocked;
  std::optional<TaskResult> result;
};

// Pure in-memory task DAG: structural validation, derived readiness, explicit
// state transitions and all-or-nothing appends. No IO, no session, no clocks;
// the owner of this object is responsible for serializing access.
class TaskGraph {
 public:
  // Validates every node before any state is created, so a rejected plan
  // leaves no partial graph behind.
  //
  // Rejects: an empty plan, invalid/duplicate ids, agents outside
  // `allowed_agents`, `depends_on` referencing unknown ids or the node itself,
  // cycles, more than `limits.max_tasks` nodes, empty or overlong goals, `fork`
  // for agents outside `fork_agents`, and plans without at least one root node.
  [[nodiscard]] static absl::StatusOr<TaskGraph> Create(
      std::vector<TaskNode> nodes, const TaskGraphLimits& limits,
      std::vector<std::string> allowed_agents,
      std::vector<std::string> fork_agents);

  // Appends nodes after re-validating the merged plan (ids unique across the
  // whole graph, dependencies may target existing or appended nodes, acyclic,
  // node count within limits). All-or-nothing: on failure nothing changes.
  [[nodiscard]] absl::Status Append(const std::vector<TaskNode>& nodes);

  // Ids in insertion order whose stored state is pending and whose
  // dependencies are all completed.
  [[nodiscard]] std::vector<std::string> ReadyIds() const;

  // Ids in insertion order that are pending but not ready: at least one
  // dependency is still pending/running, or is failed/cancelled.
  [[nodiscard]] std::vector<std::string> BlockedIds() const;

  // Dependencies of `id` that are not completed yet, in declaration order.
  // Empty for an unknown id.
  [[nodiscard]] std::vector<std::string> BlockingDependencies(
      std::string_view id) const;

  // Insertion-ordered snapshot of the whole graph.
  [[nodiscard]] std::vector<TaskNodeView> Tasks() const;
  [[nodiscard]] std::optional<TaskNodeView> Find(std::string_view id) const;

  [[nodiscard]] int size() const { return static_cast<int>(entries_.size()); }
  [[nodiscard]] bool empty() const { return entries_.empty(); }
  [[nodiscard]] bool HasRunning() const;
  [[nodiscard]] const TaskGraphLimits& limits() const { return limits_; }

  // pending (ready) -> running. Any other status is a failed precondition.
  [[nodiscard]] absl::Status MarkRunning(std::string_view id);

  // running -> completed/failed. `result.outcome` must be "completed",
  // "failed" or "blocked"; a "blocked" report is stored as failed (so
  // dependents become blocked) while the reported outcome is preserved.
  [[nodiscard]] absl::Status RecordResult(std::string_view id,
                                          TaskResult result);

  // running -> cancelled, used when the owning round is cancelled.
  [[nodiscard]] absl::Status MarkCancelled(std::string_view id);

  // failed/cancelled -> pending, dropping the previous result, so the node can
  // be delegated again once its dependencies allow it.
  [[nodiscard]] absl::Status Retry(std::string_view id);

 private:
  struct Entry {
    TaskNode node;
    TaskState state = TaskState::kPending;
    std::optional<TaskResult> result;
  };

  TaskGraph() = default;

  [[nodiscard]] TaskStatus StatusOf(const Entry& entry) const;
  [[nodiscard]] Entry* Lookup(std::string_view id);
  [[nodiscard]] const Entry* Lookup(std::string_view id) const;

  TaskGraphLimits limits_;
  std::vector<std::string> allowed_agents_;
  std::vector<std::string> fork_agents_;
  std::vector<Entry> entries_;
  // id -> index into entries_. Nothing is erased, so indices stay valid.
  std::unordered_map<std::string, std::size_t> index_;
};

}  // namespace agentflow::workflow

#endif  // AGENTFLOW_WORKFLOW_TASK_GRAPH_H_
