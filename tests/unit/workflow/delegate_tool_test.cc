#include "agentflow/workflow/delegate_tool.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/tools/tool.h"

namespace agentflow::workflow {
namespace {

TEST(DelegateToolTest, SchemaEnumeratesAllowed) {
  auto tool = MakeDelegateTool(/*runtime=*/nullptr, "parent",
                                  {"planner", "researcher"}, {});
  ASSERT_NE(tool, nullptr);
  auto schema = nlohmann::json::parse(tool->Schema().params_json_schema);
  auto agent_enum = schema["properties"]["agent"]["enum"];
  ASSERT_TRUE(agent_enum.is_array());
  EXPECT_EQ(agent_enum.size(), 2u);
  EXPECT_EQ(agent_enum[0].get<std::string>(), "planner");
  EXPECT_EQ(agent_enum[1].get<std::string>(), "researcher");
  EXPECT_EQ(tool->Schema().name, "delegate");
}

TEST(DelegateToolTest, DescriptionPresent) {
  auto tool = MakeDelegateTool(nullptr, "p", {"c"}, {});
  EXPECT_FALSE(tool->Schema().description.empty());
}

}  // namespace
}  // namespace agentflow::workflow
