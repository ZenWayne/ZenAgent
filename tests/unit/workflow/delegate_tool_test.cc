#include "agentflow/workflow/delegate_tool.h"

#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
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

std::string Invoke(::agentflow::Tool& tool, std::string args) {
  asio::io_context io;
  auto fut = asio::co_spawn(
      io,
      [&]() -> asio::awaitable<std::string> {
        ::agentflow::CancelSource cancel;
        co_return co_await tool.Invoke(args, "", cancel.Token());
      },
      asio::use_future);
  io.run();
  return fut.get();
}

TEST(DelegateToolTest, SchemaOmitsForkWhenNoTargetIsForkable) {
  auto tool = MakeDelegateTool(nullptr, "p", {"a", "b"}, {});
  auto schema = nlohmann::json::parse(tool->Schema().params_json_schema);
  EXPECT_FALSE(schema["properties"].contains("fork"));
  EXPECT_EQ(schema["required"],
            (nlohmann::json::array({"agent", "goal"})));
}

TEST(DelegateToolTest, SchemaAdvertisesForkAndItsAllowedTargets) {
  auto tool = MakeDelegateTool(nullptr, "p", {"a", "b"}, {}, nullptr, nullptr,
                               {"b"});
  auto schema = nlohmann::json::parse(tool->Schema().params_json_schema);
  ASSERT_TRUE(schema["properties"].contains("fork"));
  EXPECT_EQ(schema["properties"]["fork"]["type"], "boolean");
  const std::string description =
      schema["properties"]["fork"]["description"].get<std::string>();
  EXPECT_NE(description.find("b"), std::string::npos);
}

TEST(DelegateToolTest, ForgedForkForANonForkableTargetIsRejected) {
  auto tool = MakeDelegateTool(nullptr, "p", {"a", "b"}, {}, nullptr, nullptr,
                               {"b"});
  EXPECT_EQ(Invoke(*tool, R"({"agent":"a","goal":"x","fork":true})"),
            R"({"error":"fork_not_allowed"})");
}

TEST(DelegateToolTest, ForkForAnAllowedTargetPassesTheRosterCheck) {
  auto tool = MakeDelegateTool(nullptr, "p", {"a", "b"}, {}, nullptr, nullptr,
                               {"b"});
  // No runtime is bound, so reaching no_runtime proves the fork argument was
  // accepted for an allowed target instead of being rejected.
  EXPECT_EQ(Invoke(*tool, R"({"agent":"b","goal":"x","fork":true})"),
            R"({"error":"no_runtime"})");
}

}  // namespace
}  // namespace agentflow::workflow
