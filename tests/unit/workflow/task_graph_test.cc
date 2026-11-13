#include "agentflow/workflow/task_graph.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"

namespace agentflow::workflow {
namespace {

TaskNode Node(std::string id, std::vector<std::string> depends_on = {},
              std::string goal = "produce the artifact") {
  TaskNode node;
  node.id = std::move(id);
  node.agent = "video-agent";
  node.goal = std::move(goal);
  node.depends_on = std::move(depends_on);
  return node;
}

TaskNode ForkNode(std::string id, std::vector<std::string> depends_on = {}) {
  TaskNode node = Node(std::move(id), std::move(depends_on));
  node.agent = "analysis-agent";
  node.fork = true;
  return node;
}

std::vector<std::string> V(std::initializer_list<std::string_view> items) {
  std::vector<std::string> values;
  values.reserve(items.size());
  for (const std::string_view item : items) values.emplace_back(item);
  return values;
}

std::vector<std::string> AllowedAgents() {
  return V({"video-agent", "analysis-agent"});
}

std::vector<std::string> ForkAgents() { return V({"analysis-agent"}); }

absl::StatusOr<TaskGraph> Make(std::vector<TaskNode> nodes,
                               TaskGraphLimits limits = TaskGraphLimits{}) {
  return TaskGraph::Create(std::move(nodes), limits, AllowedAgents(),
                           ForkAgents());
}

// Asserts the plan was rejected as invalid and the message mentions every
// fragment, so a test fails for the intended reason rather than any rejection.
void ExpectRejected(const absl::StatusOr<TaskGraph>& graph,
                    std::initializer_list<std::string_view> fragments) {
  ASSERT_FALSE(graph.ok());
  EXPECT_EQ(graph.status().code(), absl::StatusCode::kInvalidArgument)
      << graph.status();
  for (const std::string_view fragment : fragments) {
    EXPECT_TRUE(absl::StrContains(graph.status().message(), fragment))
        << "message '" << graph.status().message() << "' lacks '" << fragment
        << "'";
  }
}

void ExpectStatus(const TaskGraph& graph, std::string_view id,
                  TaskStatus expected) {
  const auto view = graph.Find(id);
  ASSERT_TRUE(view.has_value()) << "task '" << id << "' not found";
  EXPECT_EQ(view->status, expected) << "task '" << id << "'";
}

void Complete(TaskGraph* graph, std::string_view id) {
  absl::Status running = graph->MarkRunning(id);
  ASSERT_TRUE(running.ok()) << running;
  absl::Status recorded = graph->RecordResult(
      std::string(id), TaskResult{"finished " + std::string(id), "completed"});
  ASSERT_TRUE(recorded.ok()) << recorded;
}

void Fail(TaskGraph* graph, std::string_view id) {
  absl::Status running = graph->MarkRunning(id);
  ASSERT_TRUE(running.ok()) << running;
  absl::Status recorded = graph->RecordResult(
      std::string(id), TaskResult{"failed " + std::string(id), "failed"});
  ASSERT_TRUE(recorded.ok()) << recorded;
}

TEST(TaskGraphTest, CreateAcceptsValidPlanAndDerivesReadiness) {
  auto graph = Make({
      Node("script"),
      Node("shot-01", {"script"}),
      Node("shot-02", {"script"}),
  });
  ASSERT_TRUE(graph.ok()) << graph.status();
  EXPECT_EQ(graph->size(), 3);
  EXPECT_FALSE(graph->empty());
  EXPECT_FALSE(graph->HasRunning());
  EXPECT_EQ(graph->ReadyIds(), V({"script"}));
  EXPECT_EQ(graph->BlockedIds(), V({"shot-01", "shot-02"}));
  EXPECT_EQ(graph->limits().max_tasks, 24);
  EXPECT_EQ(graph->limits().max_id_length, 32);

  const auto script = graph->Find("script");
  ASSERT_TRUE(script.has_value());
  EXPECT_EQ(script->status, TaskStatus::kReady);
  EXPECT_EQ(script->node.agent, "video-agent");
  EXPECT_EQ(script->node.goal, "produce the artifact");
  EXPECT_FALSE(script->node.fork);
  EXPECT_FALSE(script->result.has_value());
  EXPECT_FALSE(graph->Find("missing").has_value());
}

TEST(TaskGraphTest, CreateRejectsEmptyPlan) {
  ExpectRejected(Make({}), {"needs at least one task"});
}

TEST(TaskGraphTest, CreateRejectsDuplicateId) {
  ExpectRejected(Make({Node("script"), Node("script")}),
                 {"duplicate task id 'script'"});
}

TEST(TaskGraphTest, CreateRejectsDuplicateIdAcrossAppendBoundary) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();
  const absl::Status status = graph->Append({Node("script")});
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(status.message(), "duplicate task id 'script'"))
      << status;
  EXPECT_EQ(graph->size(), 1);
}

TEST(TaskGraphTest, CreateRejectsInvalidIds) {
  ExpectRejected(Make({Node("Script")}), {"must match [a-z0-9-]"});
  ExpectRejected(Make({Node("")}), {"must match [a-z0-9-]"});
  ExpectRejected(Make({Node("script_1")}), {"must match [a-z0-9-]"});
  ExpectRejected(Make({Node(std::string(33, 'a'))}), {"{1,32}"});
  auto graph = Make({Node(std::string(32, 'a'))});
  ASSERT_TRUE(graph.ok()) << graph.status();
  EXPECT_EQ(graph->ReadyIds(), V({std::string(32, 'a')}));
}

TEST(TaskGraphTest, CreateRejectsUnknownDependency) {
  ExpectRejected(Make({Node("script"), Node("shot-01", {"ghost"})}),
                 {"task 'shot-01' depends on unknown task 'ghost'"});
}

TEST(TaskGraphTest, CreateRejectsSelfDependency) {
  ExpectRejected(Make({Node("script", {"script"})}),
                 {"task 'script' must not depend on itself"});
}

TEST(TaskGraphTest, CreateRejectsDependencyCycle) {
  ExpectRejected(
      Make({Node("root"), Node("left", {"right"}), Node("right", {"left"})}),
      {"dependency cycle"});
  ExpectRejected(Make({Node("root"), Node("a", {"c"}), Node("b", {"a"}),
                       Node("c", {"b"})}),
                 {"dependency cycle"});
}

TEST(TaskGraphTest, CreateRejectsPlanWithoutRootTask) {
  ExpectRejected(Make({Node("left", {"right"}), Node("right", {"left"})}),
                 {"at least one root task"});
}

TEST(TaskGraphTest, CreateRejectsTooManyTasks) {
  TaskGraphLimits limits;
  limits.max_tasks = 2;
  ExpectRejected(
      Make({Node("a"), Node("b"), Node("c")}, limits),
      {"task graph has 3 tasks", "max_tasks is 2"});
  auto graph = Make({Node("a"), Node("b", {"a"})}, limits);
  ASSERT_TRUE(graph.ok()) << graph.status();
  EXPECT_EQ(graph->size(), 2);
}

TEST(TaskGraphTest, CreateRejectsUnknownAgent) {
  TaskNode node = Node("script");
  node.agent = "ghost-agent";
  ExpectRejected(Make({node}), {"'ghost-agent'", "allowed_agents"});
}

TEST(TaskGraphTest, CreateRejectsForkAgentOutsideForkAgents) {
  TaskNode node = Node("script");
  node.fork = true;  // video-agent is delegable but not forkable.
  ExpectRejected(Make({node}), {"fork", "fork_agents"});
}

TEST(TaskGraphTest, CreateAcceptsForkForListedForkAgent) {
  auto graph = Make({ForkNode("analysis")});
  ASSERT_TRUE(graph.ok()) << graph.status();
  const auto view = graph->Find("analysis");
  ASSERT_TRUE(view.has_value());
  EXPECT_TRUE(view->node.fork);
  EXPECT_EQ(view->node.agent, "analysis-agent");
}

TEST(TaskGraphTest, CreateRejectsEmptyAndOverlongGoals) {
  ExpectRejected(Make({Node("script", {}, "")}), {"has an empty goal"});

  TaskGraphLimits limits;
  limits.max_goal_chars = 5;
  ExpectRejected(Make({Node("script", {}, "123456")}, limits),
                 {"goal has 6 characters", "limit is 5"});
  auto graph = Make({Node("script", {}, "12345")}, limits);
  ASSERT_TRUE(graph.ok()) << graph.status();
}

TEST(TaskGraphTest, CreateCountsGoalCharactersNotBytes) {
  // "中" is three UTF-8 bytes, so byte counting would reject this plan.
  std::string cjk;
  for (int i = 0; i < 8000; ++i) cjk += "\xE4\xB8\xAD";
  ASSERT_EQ(cjk.size(), 24000u);
  auto graph = Make({Node("script", {}, cjk)});
  ASSERT_TRUE(graph.ok()) << graph.status();
  EXPECT_EQ(graph->Find("script")->node.goal.size(), cjk.size());

  std::string too_long = cjk + "\xE4\xB8\xAD";
  ExpectRejected(Make({Node("script", {}, too_long)}),
                 {"goal has 8001 characters", "limit is 8000"});
}

TEST(TaskGraphTest, CreateRejectsInvalidLimits) {
  TaskGraphLimits no_tasks;
  no_tasks.max_tasks = 0;
  ExpectRejected(Make({Node("script")}, no_tasks), {"max_tasks must be positive"});

  TaskGraphLimits long_ids;
  long_ids.max_id_length = 33;
  ExpectRejected(Make({Node("script")}, long_ids), {"max_id_length must be in"});

  TaskGraphLimits no_goals;
  no_goals.max_goal_chars = 0;
  ExpectRejected(Make({Node("script")}, no_goals),
                 {"max_goal_chars must be positive"});
}

TEST(TaskGraphTest, ReadinessFollowsCompletedDependencies) {
  auto graph = Make({
      Node("script"),
      Node("shot-01", {"script"}),
      Node("shot-02", {"script"}),
      Node("timeline", {"shot-01", "shot-02"}),
  });
  ASSERT_TRUE(graph.ok()) << graph.status();
  EXPECT_EQ(graph->ReadyIds(), V({"script"}));
  EXPECT_EQ(graph->BlockedIds(), V({"shot-01", "shot-02", "timeline"}));
  EXPECT_EQ(graph->BlockingDependencies("timeline"), V({"shot-01", "shot-02"}));

  Complete(&*graph, "script");
  EXPECT_EQ(graph->ReadyIds(), V({"shot-01", "shot-02"}));
  EXPECT_EQ(graph->BlockedIds(), V({"timeline"}));
  EXPECT_TRUE(graph->BlockingDependencies("script").empty());
  EXPECT_TRUE(graph->BlockingDependencies("missing").empty());

  ASSERT_TRUE(graph->MarkRunning("shot-01").ok());
  EXPECT_TRUE(graph->HasRunning());
  EXPECT_EQ(graph->Find("shot-01")->status, TaskStatus::kRunning);
  EXPECT_EQ(graph->ReadyIds(), V({"shot-02"}));

  ASSERT_TRUE(graph->RecordResult("shot-01", {"one", "completed"}).ok());
  EXPECT_EQ(graph->ReadyIds(), V({"shot-02"}));
  EXPECT_EQ(graph->BlockedIds(), V({"timeline"}));

  Complete(&*graph, "shot-02");
  EXPECT_EQ(graph->ReadyIds(), V({"timeline"}));
  EXPECT_TRUE(graph->BlockedIds().empty());
  EXPECT_FALSE(graph->HasRunning());
  EXPECT_EQ(graph->Find("script")->result->summary, "finished script");
  EXPECT_EQ(graph->Find("script")->result->outcome, "completed");
}

TEST(TaskGraphTest, FailureBlocksDependentsButNotIndependentBranches) {
  auto graph = Make({
      Node("script"),
      Node("left", {"script"}),
      Node("right", {"script"}),
      Node("join", {"left", "right"}),
      Node("tail", {"right"}),
  });
  ASSERT_TRUE(graph.ok()) << graph.status();
  Complete(&*graph, "script");
  EXPECT_EQ(graph->ReadyIds(), V({"left", "right"}));

  Fail(&*graph, "left");
  EXPECT_EQ(graph->Find("left")->status, TaskStatus::kFailed);
  EXPECT_EQ(graph->Find("left")->result->outcome, "failed");
  // "join" waits on the failed "left"; "tail" still waits on the pending
  // "right", so both are blocked but only "join" is permanently stuck.
  EXPECT_EQ(graph->BlockedIds(), V({"join", "tail"}));
  EXPECT_EQ(graph->ReadyIds(), V({"right"}));
  ExpectStatus(*graph, "join", TaskStatus::kBlocked);
  ExpectStatus(*graph, "tail", TaskStatus::kBlocked);
  EXPECT_EQ(graph->BlockingDependencies("join"), V({"left", "right"}));

  // The independent branch still runs to completion.
  Complete(&*graph, "right");
  EXPECT_EQ(graph->ReadyIds(), V({"tail"}));
  ExpectStatus(*graph, "join", TaskStatus::kBlocked);
  EXPECT_EQ(graph->BlockingDependencies("join"), V({"left"}));
}

TEST(TaskGraphTest, BlockedOutcomeIsStoredAsFailedWithOutcomePreserved) {
  auto graph = Make({Node("script"), Node("shot", {"script"})});
  ASSERT_TRUE(graph.ok()) << graph.status();
  Complete(&*graph, "script");
  ASSERT_TRUE(graph->MarkRunning("shot").ok());
  ASSERT_TRUE(
      graph->RecordResult("shot", {"needs the missing project id", "blocked"})
          .ok());

  const auto shot = graph->Find("shot");
  ASSERT_TRUE(shot.has_value());
  EXPECT_EQ(shot->status, TaskStatus::kFailed);
  EXPECT_EQ(shot->result->outcome, "blocked");
  EXPECT_EQ(shot->result->summary, "needs the missing project id");

  auto downstream = Make({Node("script"), Node("shot", {"script"}),
                          Node("tail", {"shot"})});
  ASSERT_TRUE(downstream.ok()) << downstream.status();
  Complete(&*downstream, "script");
  ASSERT_TRUE(downstream->MarkRunning("shot").ok());
  ASSERT_TRUE(downstream->RecordResult("shot", {"blocked", "blocked"}).ok());
  ExpectStatus(*downstream, "tail", TaskStatus::kBlocked);
}

TEST(TaskGraphTest, CancellationBlocksDependentsAndRetryRestores) {
  auto graph = Make({Node("script"), Node("shot", {"script"})});
  ASSERT_TRUE(graph.ok()) << graph.status();
  ASSERT_TRUE(graph->MarkRunning("script").ok());
  ASSERT_TRUE(graph->MarkCancelled("script").ok());
  EXPECT_EQ(graph->Find("script")->status, TaskStatus::kCancelled);
  EXPECT_FALSE(graph->HasRunning());
  ExpectStatus(*graph, "shot", TaskStatus::kBlocked);

  ASSERT_TRUE(graph->Retry("script").ok());
  EXPECT_EQ(graph->Find("script")->status, TaskStatus::kReady);
  Complete(&*graph, "script");
  EXPECT_EQ(graph->ReadyIds(), V({"shot"}));
}

TEST(TaskGraphTest, RetryRestoresFailedNodeAndDropsItsResult) {
  auto graph = Make({Node("script"), Node("shot", {"script"})});
  ASSERT_TRUE(graph.ok()) << graph.status();
  Complete(&*graph, "script");
  Fail(&*graph, "shot");
  ExpectStatus(*graph, "shot", TaskStatus::kFailed);
  ASSERT_TRUE(graph->Find("shot")->result.has_value());

  ASSERT_TRUE(graph->Retry("shot").ok());
  EXPECT_EQ(graph->Find("shot")->status, TaskStatus::kReady);
  EXPECT_FALSE(graph->Find("shot")->result.has_value());
  EXPECT_EQ(graph->ReadyIds(), V({"shot"}));

  Complete(&*graph, "shot");
  EXPECT_EQ(graph->Find("shot")->status, TaskStatus::kCompleted);
  EXPECT_TRUE(graph->ReadyIds().empty());
}

TEST(TaskGraphTest, RetryRejectsNodesThatDidNotFail) {
  auto graph = Make({Node("script"), Node("shot", {"script"})});
  ASSERT_TRUE(graph.ok()) << graph.status();

  absl::Status ready = graph->Retry("script");
  EXPECT_EQ(ready.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(ready.message(), "only a failed or cancelled"))
      << ready;

  Complete(&*graph, "script");
  absl::Status completed = graph->Retry("script");
  EXPECT_EQ(completed.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(completed.message(), "completed")) << completed;

  absl::Status missing = graph->Retry("ghost");
  EXPECT_EQ(missing.code(), absl::StatusCode::kNotFound);
  EXPECT_TRUE(absl::StrContains(missing.message(), "task 'ghost' not found"))
      << missing;
}

TEST(TaskGraphTest, MarkRunningRequiresReadyTask) {
  auto graph = Make({Node("script"), Node("shot", {"script"})});
  ASSERT_TRUE(graph.ok()) << graph.status();

  absl::Status blocked = graph->MarkRunning("shot");
  EXPECT_EQ(blocked.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(blocked.message(), "is blocked")) << blocked;

  ASSERT_TRUE(graph->MarkRunning("script").ok());
  absl::Status again = graph->MarkRunning("script");
  EXPECT_EQ(again.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(again.message(), "is running")) << again;

  absl::Status missing = graph->MarkRunning("ghost");
  EXPECT_EQ(missing.code(), absl::StatusCode::kNotFound);
}

TEST(TaskGraphTest, RecordResultRequiresRunningTask) {
  auto graph = Make({Node("script"), Node("shot", {"script"})});
  ASSERT_TRUE(graph.ok()) << graph.status();

  absl::Status ready = graph->RecordResult("script", {"done", "completed"});
  EXPECT_EQ(ready.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(ready.message(), "only a running task"))
      << ready;

  ASSERT_TRUE(graph->MarkRunning("script").ok());
  absl::Status invalid = graph->RecordResult("script", {"done", "done"});
  EXPECT_EQ(invalid.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(invalid.message(),
                                "must be completed, failed or blocked"))
      << invalid;
  // A rejected outcome leaves the node running; the caller can report again.
  ExpectStatus(*graph, "script", TaskStatus::kRunning);

  ASSERT_TRUE(graph->RecordResult("script", {"done", "completed"}).ok());
  absl::Status completed = graph->RecordResult("script", {"done", "completed"});
  EXPECT_EQ(completed.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(completed.message(), "is completed"))
      << completed;

  absl::Status missing = graph->RecordResult("ghost", {"done", "completed"});
  EXPECT_EQ(missing.code(), absl::StatusCode::kNotFound);
}

TEST(TaskGraphTest, MarkCancelledRequiresRunningTask) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();

  absl::Status ready = graph->MarkCancelled("script");
  EXPECT_EQ(ready.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(ready.message(), "only a running task"))
      << ready;

  ASSERT_TRUE(graph->MarkRunning("script").ok());
  ASSERT_TRUE(graph->MarkCancelled("script").ok());
  EXPECT_EQ(graph->Find("script")->status, TaskStatus::kCancelled);

  absl::Status missing = graph->MarkCancelled("ghost");
  EXPECT_EQ(missing.code(), absl::StatusCode::kNotFound);
}

TEST(TaskGraphTest, AppendExtendsGraphAfterValidation) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();
  Complete(&*graph, "script");

  ASSERT_TRUE(graph->Append({Node("shot-01", {"script"}),
                             Node("shot-02", {"shot-01"})})
                  .ok());
  EXPECT_EQ(graph->size(), 3);
  EXPECT_EQ(graph->ReadyIds(), V({"shot-01"}));
  EXPECT_EQ(graph->BlockedIds(), V({"shot-02"}));
  EXPECT_EQ(graph->Tasks().size(), 3u);
  EXPECT_EQ(graph->Tasks()[0].node.id, "script");
  EXPECT_EQ(graph->Tasks()[1].node.id, "shot-01");
  EXPECT_EQ(graph->Tasks()[1].status, TaskStatus::kReady);
  EXPECT_EQ(graph->Tasks()[2].node.id, "shot-02");
  EXPECT_EQ(graph->Tasks()[2].status, TaskStatus::kBlocked);

  Complete(&*graph, "shot-01");
  EXPECT_EQ(graph->ReadyIds(), V({"shot-02"}));
}

TEST(TaskGraphTest, AppendAcceptsNewRootAndForkNode) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();
  ASSERT_TRUE(graph->Append({Node("audit"), ForkNode("analysis", {"script"})})
                  .ok());
  EXPECT_EQ(graph->ReadyIds(), V({"script", "audit"}));
  EXPECT_TRUE(graph->Find("analysis")->node.fork);
}

TEST(TaskGraphTest, AppendRejectsDuplicateDependencyOnlyInList) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();
  ASSERT_TRUE(graph->Append({Node("shot", {"script", "script"})}).ok());
  const auto shot = graph->Find("shot");
  ASSERT_TRUE(shot.has_value());
  EXPECT_EQ(shot->node.depends_on.size(), 1u);
  EXPECT_EQ(graph->BlockingDependencies("shot"), V({"script"}));
}

TEST(TaskGraphTest, AppendRejectsUnknownDependencyWithoutChangingGraph) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();
  Complete(&*graph, "script");

  absl::Status status = graph->Append({Node("shot", {"ghost"})});
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(status.message(), "unknown task 'ghost'"))
      << status;
  EXPECT_EQ(graph->size(), 1);
  EXPECT_EQ(graph->Find("script")->status, TaskStatus::kCompleted);
  EXPECT_TRUE(graph->ReadyIds().empty());
}

TEST(TaskGraphTest, AppendRejectsCycleWithoutChangingGraph) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();

  absl::Status status = graph->Append({Node("a", {"b"}), Node("b", {"a"})});
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(status.message(), "dependency cycle")) << status;
  EXPECT_EQ(graph->size(), 1);
  EXPECT_EQ(graph->Tasks().size(), 1u);
  EXPECT_EQ(graph->ReadyIds(), V({"script"}));
}

TEST(TaskGraphTest, AppendRejectsTaskCountOverLimitWithoutChangingGraph) {
  TaskGraphLimits limits;
  limits.max_tasks = 3;
  auto graph = Make({Node("script")}, limits);
  ASSERT_TRUE(graph.ok()) << graph.status();

  absl::Status status =
      graph->Append({Node("a"), Node("b"), Node("c", {"script"})});
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(status.message(), "max_tasks is 3")) << status;
  EXPECT_EQ(graph->size(), 1);

  ASSERT_TRUE(graph->Append({Node("a"), Node("b")}).ok());
  EXPECT_EQ(graph->size(), 3);
}

TEST(TaskGraphTest, AppendRejectsBadAgentOrForkWithoutChangingGraph) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();

  TaskNode bad_agent = Node("shot", {"script"});
  bad_agent.agent = "ghost-agent";
  absl::Status agent_status = graph->Append({bad_agent});
  EXPECT_EQ(agent_status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(agent_status.message(), "allowed_agents"))
      << agent_status;

  TaskNode bad_fork = Node("shot", {"script"});
  bad_fork.fork = true;
  absl::Status fork_status = graph->Append({bad_fork});
  EXPECT_EQ(fork_status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(fork_status.message(), "fork_agents"))
      << fork_status;

  EXPECT_EQ(graph->size(), 1);
  EXPECT_EQ(graph->ReadyIds(), V({"script"}));
}

TEST(TaskGraphTest, AppendRejectsEmptyList) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();
  const absl::Status status = graph->Append({});
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(status.message(), "at least one task")) << status;
  EXPECT_EQ(graph->size(), 1);
}

TEST(TaskGraphTest, FailedAppendStillAllowsLaterValidAppend) {
  auto graph = Make({Node("script")});
  ASSERT_TRUE(graph.ok()) << graph.status();
  ASSERT_FALSE(graph->Append({Node("shot", {"ghost"})}).ok());
  ASSERT_TRUE(graph->Append({Node("shot", {"script"})}).ok());
  EXPECT_EQ(graph->size(), 2);
  EXPECT_EQ(graph->ReadyIds(), V({"script"}));
  EXPECT_EQ(graph->BlockedIds(), V({"shot"}));
}

TEST(TaskGraphTest, NamesCoverEveryStatus) {
  EXPECT_EQ(TaskStateName(TaskState::kPending), "pending");
  EXPECT_EQ(TaskStateName(TaskState::kRunning), "running");
  EXPECT_EQ(TaskStateName(TaskState::kCompleted), "completed");
  EXPECT_EQ(TaskStateName(TaskState::kFailed), "failed");
  EXPECT_EQ(TaskStateName(TaskState::kCancelled), "cancelled");
  EXPECT_EQ(TaskStatusName(TaskStatus::kReady), "ready");
  EXPECT_EQ(TaskStatusName(TaskStatus::kBlocked), "blocked");
  EXPECT_EQ(TaskStatusName(TaskStatus::kRunning), "running");
  EXPECT_EQ(TaskStatusName(TaskStatus::kCompleted), "completed");
  EXPECT_EQ(TaskStatusName(TaskStatus::kFailed), "failed");
  EXPECT_EQ(TaskStatusName(TaskStatus::kCancelled), "cancelled");
}

}  // namespace
}  // namespace agentflow::workflow
