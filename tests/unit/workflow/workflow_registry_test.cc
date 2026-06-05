#include "agentflow/workflow/workflow_registry.h"

#include <gtest/gtest.h>
#include <asio/io_context.hpp>

#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"

namespace agentflow::workflow {
namespace {

constexpr char kJsonV1[] = R"({
  "schema_version": 1, "name":"x", "version":"v1",
  "state":{"kind":"dynamic_json","fields":{}},
  "agents":{"a":{"system_prompt":"","model":{},"tools":[]}},
  "main":"a"
})";

constexpr char kJsonV2[] = R"({
  "schema_version": 1, "name":"x", "version":"v2",
  "state":{"kind":"dynamic_json","fields":{}},
  "agents":{"a":{"system_prompt":"","model":{},"tools":[]}},
  "main":"a"
})";

TEST(WorkflowRegistryTest, RegisterAndGetLatest) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  WorkflowRegistry registry;

  auto v1 = *WorkflowLoader::Load(kJsonV1, host_tools);
  registry.Register(v1);

  auto got = registry.GetLatest("x");
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->version(), "v1");
}

TEST(WorkflowRegistryTest, RegisteringV2DoesNotEvictV1InFlight) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  WorkflowRegistry registry;

  auto v1 = *WorkflowLoader::Load(kJsonV1, host_tools);
  auto v2 = *WorkflowLoader::Load(kJsonV2, host_tools);
  auto handle = registry.Register(v1);
  registry.Register(v2);

  // Handle returned at v1's registration still serves v1.
  EXPECT_EQ(handle->version(), "v1");
  // GetLatest now returns v2.
  EXPECT_EQ(registry.GetLatest("x")->version(), "v2");
  // Pinned lookup still finds v1.
  EXPECT_EQ(registry.Get("x", "v1")->version(), "v1");
}

TEST(WorkflowRegistryTest, ListShowsAllVersions) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  WorkflowRegistry registry;
  registry.Register(*WorkflowLoader::Load(kJsonV1, host_tools));
  registry.Register(*WorkflowLoader::Load(kJsonV2, host_tools));
  auto entries = registry.List();
  EXPECT_EQ(entries.size(), 2u);
}

TEST(WorkflowRegistryTest, UnregisterRemovesEntry) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  WorkflowRegistry registry;
  registry.Register(*WorkflowLoader::Load(kJsonV1, host_tools));
  EXPECT_TRUE(registry.Unregister("x", "v1"));
  EXPECT_EQ(registry.GetLatest("x"), nullptr);
}

TEST(WorkflowRegistryTest, UnregisterLatestPromotesNextNewest) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  WorkflowRegistry registry;
  registry.Register(*WorkflowLoader::Load(kJsonV1, host_tools));
  registry.Register(*WorkflowLoader::Load(kJsonV2, host_tools));
  // GetLatest sees v2.
  EXPECT_EQ(registry.GetLatest("x")->version(), "v2");
  EXPECT_TRUE(registry.Unregister("x", "v2"));
  // After unregistering v2, v1 takes over as latest.
  ASSERT_NE(registry.GetLatest("x"), nullptr);
  EXPECT_EQ(registry.GetLatest("x")->version(), "v1");
}

}  // namespace
}  // namespace agentflow::workflow
