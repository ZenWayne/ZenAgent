#include "agentflow/workflow/workflow_runner.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <asio/io_context.hpp>

#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "tests/support/fake_chat_backend.h"

namespace agentflow::workflow {
namespace {

constexpr char kRosterJson[] = R"({
  "schema_version":1,"name":"t","version":"v1",
  "state":{"kind":"dynamic_json","fields":{}},
  "agents":{
    "parent":{"system_prompt":"You delegate.","model":{"max_output_tokens":128},"tools":[],
              "delegates":{"agents":["worker"],"max_depth":2}},
    "worker":{"system_prompt":"Do work.","model":{},"tools":[]}
  },
  "main":"parent"
})";

constexpr char kSoloJson[] = R"({
  "schema_version":1,"name":"t","version":"v1",
  "state":{"kind":"dynamic_json","fields":{}},
  "agents":{"solo":{"system_prompt":"Be brief.","model":{},"tools":[]}},
  "main":"solo"
})";

TEST(WorkflowRunnerTest, BuildsParentWithDelegateExtra) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, *tools);

  AgentNodeBuildSpec spec;
  spec.workflow = wf;
  spec.agent_name = "parent";
  spec.host_tools = tools;
  // No backend + no io_ctx: delegate tool is NOT attached (sub-agent runtime
  // requires a live backend).
  auto built = BuildAgentNode(spec);
  EXPECT_EQ(built.cfg.system_prompt, "You delegate.");
  EXPECT_TRUE(built.cfg.extra_tools.empty());
}

TEST(WorkflowRunnerTest, BuildsParentWithDelegateWhenEngineProvided) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>(io);
  auto wf = *WorkflowLoader::Load(kRosterJson, *tools);

  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});

  AgentNodeBuildSpec spec;
  spec.workflow = wf;
  spec.agent_name = "parent";
  spec.host_tools = tools;
  spec.backend = backend;
  spec.io_ctx = &io;
  auto built = BuildAgentNode(spec);
  ASSERT_EQ(built.cfg.extra_tools.size(), 1u);
  EXPECT_EQ(built.cfg.extra_tools[0]->Schema().name, "delegate");
  // Keepalive holds the runtime + delegate tool.
  EXPECT_EQ(built.keepalive.size(), 2u);
}

TEST(WorkflowRunnerTest, NoDelegateForSoloAgent) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>(io);
  auto wf = *WorkflowLoader::Load(kSoloJson, *tools);

  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  AgentNodeBuildSpec spec;
  spec.workflow = wf;
  spec.agent_name = "solo";
  spec.host_tools = tools;
  spec.backend = backend;
  spec.io_ctx = &io;
  auto built = BuildAgentNode(spec);
  EXPECT_TRUE(built.cfg.extra_tools.empty());
  EXPECT_TRUE(built.keepalive.empty());
  EXPECT_EQ(built.cfg.system_prompt, "Be brief.");
}

TEST(WorkflowRunnerTest, UnknownAgentReturnsEmptyConfig) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>(io);
  auto wf = *WorkflowLoader::Load(kSoloJson, *tools);

  AgentNodeBuildSpec spec;
  spec.workflow = wf;
  spec.agent_name = "ghost";
  spec.host_tools = tools;
  auto built = BuildAgentNode(spec);
  EXPECT_TRUE(built.cfg.system_prompt.empty());
  EXPECT_TRUE(built.cfg.extra_tools.empty());
}

}  // namespace
}  // namespace agentflow::workflow
