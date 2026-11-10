#include "agentflow/workflow/task_graph.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace agentflow::workflow {
namespace {

// Hard ceiling for task ids, independent of the configurable limit.
constexpr std::size_t kMaxTaskIdBytes = 32;

bool IsAllowedTaskIdChar(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
}

bool IsValidTaskId(std::string_view id, int max_length) {
  if (id.empty() || id.size() > static_cast<std::size_t>(max_length)) {
    return false;
  }
  for (const char ch : id) {
    if (!IsAllowedTaskIdChar(ch)) return false;
  }
  return true;
}

// Counts UTF-8 code points so a goal limit in "characters" does not depend on
// the script the goal is written in. Invalid continuation bytes are ignored
// rather than rejected; goal text comes from the model, not from a parser.
std::size_t CountUtf8CodePoints(std::string_view text) {
  std::size_t count = 0;
  for (const char ch : text) {
    if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++count;
  }
  return count;
}

bool Contains(const std::vector<std::string>& values, std::string_view value) {
  for (const std::string& candidate : values) {
    if (candidate == value) return true;
  }
  return false;
}

// Collapses repeated dependency entries, keeping the first occurrence order.
// A repeated dependency is harmless semantically, so it is normalized instead
// of rejected.
void NormalizeNodes(std::vector<TaskNode>* nodes) {
  for (TaskNode& node : *nodes) {
    std::vector<std::string> unique;
    unique.reserve(node.depends_on.size());
    for (std::string& dependency : node.depends_on) {
      bool seen = false;
      for (const std::string& kept : unique) {
        if (kept == dependency) {
          seen = true;
          break;
        }
      }
      if (!seen) unique.push_back(std::move(dependency));
    }
    node.depends_on = std::move(unique);
  }
}

std::string JoinIds(const std::vector<std::string>& ids) {
  std::string joined;
  for (const std::string& id : ids) {
    if (!joined.empty()) joined += ", ";
    joined += id;
  }
  return joined;
}

// Structural validation only: it inspects the node list, never the runtime
// state, so the same code validates a fresh plan and an appended one.
absl::Status ValidateGraph(const std::vector<TaskNode>& nodes,
                           const TaskGraphLimits& limits,
                           const std::vector<std::string>& allowed_agents,
                           const std::vector<std::string>& fork_agents) {
  if (nodes.empty()) {
    return absl::InvalidArgumentError(
        "task plan needs at least one task");
  }
  if (nodes.size() > static_cast<std::size_t>(limits.max_tasks)) {
    return absl::InvalidArgumentError(
        absl::StrCat("task graph has ", nodes.size(), " tasks, max_tasks is ",
                     limits.max_tasks));
  }
  std::unordered_map<std::string, std::size_t> by_id;
  by_id.reserve(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const TaskNode& node = nodes[index];
    if (!IsValidTaskId(node.id, limits.max_id_length)) {
      return absl::InvalidArgumentError(
          absl::StrCat("task id '", node.id, "' must match [a-z0-9-]{1,",
                       limits.max_id_length, "}"));
    }
    if (!by_id.emplace(node.id, index).second) {
      return absl::InvalidArgumentError(
          absl::StrCat("duplicate task id '", node.id, "'"));
    }
    if (!Contains(allowed_agents, node.agent)) {
      return absl::InvalidArgumentError(
          absl::StrCat("task '", node.id, "' targets agent '", node.agent,
                       "', which is not in allowed_agents"));
    }
    if (node.goal.empty()) {
      return absl::InvalidArgumentError(
          absl::StrCat("task '", node.id, "' has an empty goal"));
    }
    const std::size_t goal_chars = CountUtf8CodePoints(node.goal);
    if (goal_chars > limits.max_goal_chars) {
      return absl::InvalidArgumentError(
          absl::StrCat("task '", node.id, "' goal has ", goal_chars,
                       " characters, limit is ", limits.max_goal_chars));
    }
    if (node.fork && !Contains(fork_agents, node.agent)) {
      return absl::InvalidArgumentError(
          absl::StrCat("task '", node.id, "' sets fork for agent '",
                       node.agent, "', which is not in fork_agents"));
    }
  }

  for (const TaskNode& node : nodes) {
    for (const std::string& dependency : node.depends_on) {
      if (dependency == node.id) {
        return absl::InvalidArgumentError(
            absl::StrCat("task '", node.id, "' must not depend on itself"));
      }
      if (by_id.find(dependency) == by_id.end()) {
        return absl::InvalidArgumentError(
            absl::StrCat("task '", node.id, "' depends on unknown task '",
                         dependency, "'"));
      }
    }
  }

  // Checked before the cycle walk: every rootless graph also contains a cycle,
  // and "no root task" is the more useful diagnosis for that shape.
  bool has_root = false;
  for (const TaskNode& node : nodes) {
    if (node.depends_on.empty()) {
      has_root = true;
      break;
    }
  }
  if (!has_root) {
    return absl::InvalidArgumentError(
        "task graph needs at least one root task with no dependencies");
  }

  std::vector<int> indegree(nodes.size(), 0);
  std::vector<std::vector<std::size_t>> dependents(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    for (const std::string& dependency : nodes[index].depends_on) {
      const std::size_t source = by_id.at(dependency);
      ++indegree[index];
      dependents[source].push_back(index);
    }
  }
  std::vector<std::size_t> frontier;
  frontier.reserve(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (indegree[index] == 0) frontier.push_back(index);
  }
  std::size_t visited = 0;
  for (std::size_t cursor = 0; cursor < frontier.size(); ++cursor) {
    const std::size_t current = frontier[cursor];
    ++visited;
    for (const std::size_t dependent : dependents[current]) {
      if (--indegree[dependent] == 0) frontier.push_back(dependent);
    }
  }
  if (visited != nodes.size()) {
    std::vector<std::string> cyclic;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      if (indegree[index] > 0) cyclic.push_back(nodes[index].id);
    }
    return absl::InvalidArgumentError(
        absl::StrCat("task graph has a dependency cycle involving '",
                     JoinIds(cyclic), "'"));
  }
  return absl::OkStatus();
}

}  // namespace

std::string_view TaskStateName(TaskState state) {
  switch (state) {
    case TaskState::kPending:
      return "pending";
    case TaskState::kRunning:
      return "running";
    case TaskState::kCompleted:
      return "completed";
    case TaskState::kFailed:
      return "failed";
    case TaskState::kCancelled:
      return "cancelled";
  }
  return "unknown";
}

std::string_view TaskStatusName(TaskStatus status) {
  switch (status) {
    case TaskStatus::kReady:
      return "ready";
    case TaskStatus::kBlocked:
      return "blocked";
    case TaskStatus::kRunning:
      return "running";
    case TaskStatus::kCompleted:
      return "completed";
    case TaskStatus::kFailed:
      return "failed";
    case TaskStatus::kCancelled:
      return "cancelled";
  }
  return "unknown";
}

absl::StatusOr<TaskGraph> TaskGraph::Create(
    std::vector<TaskNode> nodes, const TaskGraphLimits& limits,
    std::vector<std::string> allowed_agents,
    std::vector<std::string> fork_agents) {
  if (limits.max_tasks <= 0) {
    return absl::InvalidArgumentError("max_tasks must be positive");
  }
  if (limits.max_id_length <= 0 ||
      static_cast<std::size_t>(limits.max_id_length) > kMaxTaskIdBytes) {
    return absl::InvalidArgumentError(
        absl::StrCat("max_id_length must be in [1, ", kMaxTaskIdBytes, "]"));
  }
  if (limits.max_goal_chars == 0) {
    return absl::InvalidArgumentError("max_goal_chars must be positive");
  }
  NormalizeNodes(&nodes);
  if (absl::Status status =
          ValidateGraph(nodes, limits, allowed_agents, fork_agents);
      !status.ok()) {
    return status;
  }

  TaskGraph graph;
  graph.limits_ = limits;
  graph.allowed_agents_ = std::move(allowed_agents);
  graph.fork_agents_ = std::move(fork_agents);
  graph.entries_.reserve(nodes.size());
  graph.index_.reserve(nodes.size());
  for (TaskNode& node : nodes) {
    const std::size_t index = graph.entries_.size();
    graph.index_.emplace(node.id, index);
    graph.entries_.push_back(
        Entry{std::move(node), TaskState::kPending, std::nullopt});
  }
  return graph;
}

absl::Status TaskGraph::Append(const std::vector<TaskNode>& nodes) {
  if (nodes.empty()) {
    return absl::InvalidArgumentError("append needs at least one task");
  }
  std::vector<TaskNode> added = nodes;
  NormalizeNodes(&added);
  std::vector<TaskNode> merged;
  merged.reserve(entries_.size() + added.size());
  for (const Entry& entry : entries_) merged.push_back(entry.node);
  merged.insert(merged.end(), added.begin(), added.end());
  if (absl::Status status =
          ValidateGraph(merged, limits_, allowed_agents_, fork_agents_);
      !status.ok()) {
    return status;
  }
  for (TaskNode& node : added) {
    const std::size_t index = entries_.size();
    index_.emplace(node.id, index);
    entries_.push_back(
        Entry{std::move(node), TaskState::kPending, std::nullopt});
  }
  return absl::OkStatus();
}

TaskStatus TaskGraph::StatusOf(const Entry& entry) const {
  switch (entry.state) {
    case TaskState::kRunning:
      return TaskStatus::kRunning;
    case TaskState::kCompleted:
      return TaskStatus::kCompleted;
    case TaskState::kFailed:
      return TaskStatus::kFailed;
    case TaskState::kCancelled:
      return TaskStatus::kCancelled;
    case TaskState::kPending:
      break;
  }
  for (const std::string& dependency : entry.node.depends_on) {
    const Entry* upstream = Lookup(dependency);
    if (upstream == nullptr || upstream->state != TaskState::kCompleted) {
      return TaskStatus::kBlocked;
    }
  }
  return TaskStatus::kReady;
}

std::vector<std::string> TaskGraph::ReadyIds() const {
  std::vector<std::string> ready;
  for (const Entry& entry : entries_) {
    if (StatusOf(entry) == TaskStatus::kReady) ready.push_back(entry.node.id);
  }
  return ready;
}

std::vector<std::string> TaskGraph::BlockedIds() const {
  std::vector<std::string> blocked;
  for (const Entry& entry : entries_) {
    if (StatusOf(entry) == TaskStatus::kBlocked) blocked.push_back(entry.node.id);
  }
  return blocked;
}

std::vector<std::string> TaskGraph::BlockingDependencies(
    std::string_view id) const {
  std::vector<std::string> blocking;
  const Entry* entry = Lookup(id);
  if (entry == nullptr) return blocking;
  for (const std::string& dependency : entry->node.depends_on) {
    const Entry* upstream = Lookup(dependency);
    if (upstream == nullptr || upstream->state != TaskState::kCompleted) {
      blocking.push_back(dependency);
    }
  }
  return blocking;
}

std::vector<TaskNodeView> TaskGraph::Tasks() const {
  std::vector<TaskNodeView> views;
  views.reserve(entries_.size());
  for (const Entry& entry : entries_) {
    views.push_back(TaskNodeView{entry.node, StatusOf(entry), entry.result});
  }
  return views;
}

std::optional<TaskNodeView> TaskGraph::Find(std::string_view id) const {
  const Entry* entry = Lookup(id);
  if (entry == nullptr) return std::nullopt;
  return TaskNodeView{entry->node, StatusOf(*entry), entry->result};
}

bool TaskGraph::HasRunning() const {
  for (const Entry& entry : entries_) {
    if (entry.state == TaskState::kRunning) return true;
  }
  return false;
}

absl::Status TaskGraph::MarkRunning(std::string_view id) {
  Entry* entry = Lookup(id);
  if (entry == nullptr) {
    return absl::NotFoundError(absl::StrCat("task '", id, "' not found"));
  }
  const TaskStatus status = StatusOf(*entry);
  if (status != TaskStatus::kReady) {
    return absl::FailedPreconditionError(
        absl::StrCat("task '", id, "' is ", TaskStatusName(status),
                     "; only a ready task can start"));
  }
  entry->state = TaskState::kRunning;
  return absl::OkStatus();
}

absl::Status TaskGraph::RecordResult(std::string_view id, TaskResult result) {
  Entry* entry = Lookup(id);
  if (entry == nullptr) {
    return absl::NotFoundError(absl::StrCat("task '", id, "' not found"));
  }
  if (entry->state != TaskState::kRunning) {
    return absl::FailedPreconditionError(
        absl::StrCat("task '", id, "' is ",
                     TaskStateName(entry->state),
                     "; only a running task can record a result"));
  }
  if (result.outcome != "completed" && result.outcome != "failed" &&
      result.outcome != "blocked") {
    return absl::InvalidArgumentError(
        absl::StrCat("task '", id, "' result outcome '", result.outcome,
                     "' must be completed, failed or blocked"));
  }
  entry->state = result.outcome == "completed" ? TaskState::kCompleted
                                               : TaskState::kFailed;
  entry->result = std::move(result);
  return absl::OkStatus();
}

absl::Status TaskGraph::MarkCancelled(std::string_view id) {
  Entry* entry = Lookup(id);
  if (entry == nullptr) {
    return absl::NotFoundError(absl::StrCat("task '", id, "' not found"));
  }
  if (entry->state != TaskState::kRunning) {
    return absl::FailedPreconditionError(
        absl::StrCat("task '", id, "' is ", TaskStateName(entry->state),
                     "; only a running task can be cancelled"));
  }
  entry->state = TaskState::kCancelled;
  return absl::OkStatus();
}

absl::Status TaskGraph::Retry(std::string_view id) {
  Entry* entry = Lookup(id);
  if (entry == nullptr) {
    return absl::NotFoundError(absl::StrCat("task '", id, "' not found"));
  }
  if (entry->state != TaskState::kFailed &&
      entry->state != TaskState::kCancelled) {
    return absl::FailedPreconditionError(
        absl::StrCat("task '", id, "' is ", TaskStateName(entry->state),
                     "; only a failed or cancelled task can be retried"));
  }
  entry->state = TaskState::kPending;
  entry->result.reset();
  return absl::OkStatus();
}

TaskGraph::Entry* TaskGraph::Lookup(std::string_view id) {
  const auto it = index_.find(std::string(id));
  if (it == index_.end()) return nullptr;
  return &entries_[it->second];
}

const TaskGraph::Entry* TaskGraph::Lookup(std::string_view id) const {
  const auto it = index_.find(std::string(id));
  if (it == index_.end()) return nullptr;
  return &entries_[it->second];
}

}  // namespace agentflow::workflow
