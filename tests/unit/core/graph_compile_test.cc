// tests/unit/core/graph_compile_test.cc
#include "agentflow/core/graph.h"

#include <algorithm>
#include <unordered_map>

#include <gtest/gtest.h>

#include "tests/unit/core/stub_node.h"

namespace agentflow {
namespace {

std::unique_ptr<Node> MakeStub(std::string id) {
  return std::make_unique<StubNode>(std::move(id),
                                     std::chrono::milliseconds(0),
                                     nullptr);
}

TEST(GraphCompileTest, LinearDagAllGroupZero) {
  GraphBuilder b;
  b.AddNode(MakeStub("a"))
   .AddNode(MakeStub("b"))
   .AddNode(MakeStub("c"))
   .AddEdge("a", "b").AddEdge("b", "c");
  auto g = b.Build();
  for (const auto& e : g.Edges()) {
    EXPECT_EQ(e.activation_group, 0) << e.from << "->" << e.to;
  }
  ASSERT_EQ(g.EntryNodeIds().size(), 1u);
  EXPECT_EQ(g.EntryNodeIds()[0], "a");
}

TEST(GraphCompileTest, DiamondAllGroupZero) {
  GraphBuilder b;
  b.AddNode(MakeStub("a")).AddNode(MakeStub("b"))
   .AddNode(MakeStub("c")).AddNode(MakeStub("d"))
   .AddEdge("a", "b").AddEdge("a", "c")
   .AddEdge("b", "d").AddEdge("c", "d");
  auto g = b.Build();
  for (const auto& e : g.Edges()) EXPECT_EQ(e.activation_group, 0);
}

TEST(GraphCompileTest, UserGroupPreservedOnSelfLoop) {
  GraphBuilder b;
  b.AddNode(MakeStub("a")).AddNode(MakeStub("b"))
   .AddEdge("a", "b")
   .AddEdge("b", "b", /*user_group=*/1, Edge::Condition::ALL);
  auto g = b.Build();
  int self_group = -1, ext_group = -1;
  for (const auto& e : g.Edges()) {
    if (e.from == "b" && e.to == "b") self_group = e.activation_group;
    if (e.from == "a" && e.to == "b") ext_group = e.activation_group;
  }
  EXPECT_EQ(self_group, 1);
  EXPECT_EQ(ext_group, 0);
}

TEST(GraphCompileTest, TwoNodeCycleUsesUserGroup) {
  GraphBuilder b;
  b.AddNode(MakeStub("start")).AddNode(MakeStub("a")).AddNode(MakeStub("b"))
   .AddEdge("start", "a")
   .AddEdge("a", "b", 1, Edge::Condition::ALL)
   .AddEdge("b", "a", 1, Edge::Condition::ALL);
  auto g = b.Build();
  int ab = -1, ba = -1, start_a = -1;
  for (const auto& e : g.Edges()) {
    if (e.from == "a" && e.to == "b") ab = e.activation_group;
    if (e.from == "b" && e.to == "a") ba = e.activation_group;
    if (e.from == "start" && e.to == "a") start_a = e.activation_group;
  }
  EXPECT_EQ(ab, 1);
  EXPECT_EQ(ba, 1);
  EXPECT_EQ(start_a, 0);
  // Both `start` (no incoming) and `b` (only group=1 incoming) qualify as
  // entries — `b` bootstraps via the Runner's times_fired==0 rule.
  EXPECT_EQ(g.EntryNodeIds().size(), 2u);
  std::vector<std::string> entries = g.EntryNodeIds();
  EXPECT_NE(std::find(entries.begin(), entries.end(), "start"), entries.end());
  EXPECT_NE(std::find(entries.begin(), entries.end(), "b"), entries.end());
}

TEST(GraphCompileTest, TwoSeparateCyclesUseDistinctUserGroups) {
  GraphBuilder b;
  for (auto id : {"start", "a", "b", "c", "d"}) b.AddNode(MakeStub(id));
  b.AddEdge("start", "a")
   .AddEdge("a", "b", 1, Edge::Condition::ALL)
   .AddEdge("b", "a", 1, Edge::Condition::ALL);
  b.AddEdge("start", "c")
   .AddEdge("c", "d", 2, Edge::Condition::ALL)
   .AddEdge("d", "c", 2, Edge::Condition::ALL);
  auto g = b.Build();
  int ab = 0, cd = 0;
  for (const auto& e : g.Edges()) {
    if (e.from == "a" && e.to == "b") ab = e.activation_group;
    if (e.from == "c" && e.to == "d") cd = e.activation_group;
  }
  EXPECT_EQ(ab, 1);
  EXPECT_EQ(cd, 2);
}

TEST(GraphCompileTest, CycleOnlyIncomingNodeIsEntry) {
  GraphBuilder b;
  b.AddNode(MakeStub("a")).AddNode(MakeStub("b"))
   .AddEdge("a", "b", 1, Edge::Condition::ALL)
   .AddEdge("b", "a", 1, Edge::Condition::ALL);
  auto g = b.Build();
  EXPECT_EQ(g.EntryNodeIds().size(), 2u);
}

TEST(GraphCompileTest, ConflictingConditionThrows) {
  // Two non-cycle (group=0) edges into the same node with conflicting
  // ALL/ANY conditions — same as autogen's "different activation groups
  // detection" scenario for the default group.
  GraphBuilder b;
  b.AddNode(MakeStub("a")).AddNode(MakeStub("b")).AddNode(MakeStub("c"))
   .AddEdge("a", "c", Edge::Condition::ALL)
   .AddEdge("b", "c", Edge::Condition::ANY);
  EXPECT_THROW(b.Build(), GraphCompileError);
}

TEST(GraphCompileTest, DifferentGroupsAllowDifferentConditions) {
  // Same target, two distinct cycle groups — each group can carry its own
  // condition. group=1 ALL, group=2 ANY on node `target` should compile.
  GraphBuilder b;
  for (auto id : {"start", "a1", "b1", "a2", "b2", "target"})
    b.AddNode(MakeStub(id));
  // Cycle 1 (group=1, ALL) feeds target via b1 -> target.
  b.AddEdge("start", "a1")
   .AddEdge("a1", "b1", 1, Edge::Condition::ALL)
   .AddEdge("b1", "a1", 1, Edge::Condition::ALL)
   .AddEdge("b1", "target", 1, Edge::Condition::ALL);
  // Cycle 2 (group=2, ANY) feeds target via b2 -> target.
  b.AddEdge("start", "a2")
   .AddEdge("a2", "b2", 2, Edge::Condition::ANY)
   .AddEdge("b2", "a2", 2, Edge::Condition::ANY)
   .AddEdge("b2", "target", 2, Edge::Condition::ANY);
  auto g = b.Build();
  // No throw = different groups truly isolate condition validation.
  // Verify the edges into `target` have distinct groups + distinct conditions.
  std::unordered_map<int, Edge::Condition> by_group;
  for (const auto& e : g.Edges()) {
    if (e.to == "target") by_group[e.activation_group] = e.condition;
  }
  ASSERT_EQ(by_group.size(), 2u);
  EXPECT_EQ(by_group.at(1), Edge::Condition::ALL);
  EXPECT_EQ(by_group.at(2), Edge::Condition::ANY);
}

TEST(GraphCompileTest, ConflictingConditionWithinSameCycleGroupThrows) {
  // Two edges into the same node, both in cycle group=1, but with
  // conflicting ALL/ANY conditions — must be rejected just like group=0.
  GraphBuilder b;
  b.AddNode(MakeStub("entry")).AddNode(MakeStub("a"))
   .AddNode(MakeStub("b")).AddNode(MakeStub("c"))
   .AddEdge("entry", "a")
   .AddEdge("a", "c", 1, Edge::Condition::ALL)
   .AddEdge("b", "c", 1, Edge::Condition::ANY)
   .AddEdge("c", "a", 1, Edge::Condition::ALL)
   .AddEdge("c", "b", 1, Edge::Condition::ANY);
  EXPECT_THROW(b.Build(), GraphCompileError);
}

TEST(GraphCompileTest, DuplicateNodeIdThrows) {
  GraphBuilder b;
  b.AddNode(MakeStub("dup")).AddNode(MakeStub("dup"));
  EXPECT_THROW(b.Build(), GraphCompileError);
}

TEST(GraphCompileTest, EdgeReferencingUnknownNodeThrows) {
  GraphBuilder b;
  b.AddNode(MakeStub("a")).AddEdge("a", "ghost");
  EXPECT_THROW(b.Build(), GraphCompileError);
}

TEST(GraphCompileTest, NoEntryNodeThrows) {
  GraphBuilder b;
  b.AddNode(MakeStub("a")).AddNode(MakeStub("b"))
   .AddEdge("a", "b").AddEdge("b", "a");
  EXPECT_THROW(b.Build(), GraphCompileError);
}

TEST(GraphCompileTest, UserGroupZeroRejected) {
  GraphBuilder b;
  b.AddNode(MakeStub("a")).AddNode(MakeStub("b"));
  EXPECT_THROW(b.AddEdge("a", "b", 0, Edge::Condition::ALL),
               GraphCompileError);
}

TEST(GraphCompileTest, DotStringIncludesGroupLabels) {
  GraphBuilder b;
  b.AddNode(MakeStub("a")).AddNode(MakeStub("b"))
   .AddEdge("a", "b")
   .AddEdge("b", "a", 1, Edge::Condition::ALL);
  auto g = b.Build();
  std::string dot = g.ToDotString();
  EXPECT_NE(dot.find("g=1"), std::string::npos);
  EXPECT_NE(dot.find("g=0"), std::string::npos);
  EXPECT_NE(dot.find("\"a\" -> \"b\""), std::string::npos);
  EXPECT_NE(dot.find("\"b\" -> \"a\""), std::string::npos);
}

}  // namespace
}  // namespace agentflow
