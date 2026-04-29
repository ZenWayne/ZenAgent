// tests/unit/core/graph_compile_test.cc
#include "agentflow/core/graph.h"

#include <algorithm>

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
  GraphBuilder b;
  b.AddNode(MakeStub("a")).AddNode(MakeStub("b")).AddNode(MakeStub("c"))
   .AddEdge("a", "c", Edge::Condition::ALL)
   .AddEdge("b", "c", Edge::Condition::ANY);
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
