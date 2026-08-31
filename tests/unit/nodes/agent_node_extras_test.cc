// tests/unit/nodes/agent_node_extras_test.cc
#include "agentflow/nodes/agent_node.h"

#include <asio/io_context.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/tools/native_fn_tool.h"

namespace agentflow {
namespace {

TEST(AgentNodeExtrasTest, ConfigAcceptsExtraTools) {
  ToolSchema schema{"echo", "echoes args back", R"({"type":"object"})"};
  auto extra = std::make_shared<NativeFnTool>(
      schema,
      [](std::string_view args, std::string_view, const CancelToken&)
          -> asio::awaitable<std::string> {
        co_return std::string(args);
      });
  AgentNodeConfig cfg;
  cfg.extra_tools.push_back(extra);
  // Just verify the field is held; runtime exercise requires an engine.
  EXPECT_EQ(cfg.extra_tools.size(), 1u);
  EXPECT_EQ(cfg.extra_tools[0]->Schema().name, "echo");
}

}  // namespace
}  // namespace agentflow
