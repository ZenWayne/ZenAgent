// tests/unit/workflow/backend_selection_test.cc
//
// ModelSpec.backend names a logical backend the host registered. Credentials
// and base URLs live in the host-constructed instance, never in the spec.
#include <memory>
#include <string>
#include <vector>

#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/core/errors.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow_loader.h"
#include "agentflow/workflow/workflow_runner.h"
#include "tests/support/fake_chat_backend.h"

namespace agentflow::workflow {
namespace {

// Minimal two-agent workflow: `local` takes the default backend, `cloud`
// selects a named one.
constexpr char kWorkflowJson[] = R"({
  "name": "backend-selection",
  "version": "1",
  "state": {"fields": {}},
  "agents": {
    "local": {"system_prompt": "local agent"},
    "cloud": {"system_prompt": "cloud agent",
              "model": {"backend": "cloud-big"}}
  },
  "main": "local"
})";

std::shared_ptr<Workflow> LoadWorkflow(const ToolRegistry& tools) {
  auto wf_or = WorkflowLoader::Load(kWorkflowJson, tools);
  EXPECT_TRUE(wf_or.ok()) << wf_or.status().message();
  return *wf_or;
}

AgentNodeBuildSpec MakeSpec(std::shared_ptr<Workflow> wf,
                             std::shared_ptr<ToolRegistry> tools,
                             asio::io_context& io) {
  AgentNodeBuildSpec spec;
  spec.workflow = std::move(wf);
  spec.host_tools = std::move(tools);
  spec.io_ctx = &io;
  return spec;
}

TEST(BackendSelectionTest, EmptyBackendNameUsesTheDefault) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>();
  auto spec = MakeSpec(LoadWorkflow(*tools), tools, io);

  auto fallback = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  spec.backend = fallback;
  spec.agent_name = "local";

  auto built = BuildAgentNode(spec);
  EXPECT_EQ(built.cfg.backend, fallback);
}

TEST(BackendSelectionTest, NamedBackendIsResolvedFromTheMap) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>();
  auto spec = MakeSpec(LoadWorkflow(*tools), tools, io);

  auto fallback = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  auto cloud = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  spec.backend = fallback;
  spec.backends["cloud-big"] = cloud;
  spec.agent_name = "cloud";

  auto built = BuildAgentNode(spec);
  EXPECT_EQ(built.cfg.backend, cloud);
  EXPECT_NE(built.cfg.backend, fallback);
}

TEST(BackendSelectionTest, UnknownBackendNameThrowsRatherThanFallingBack) {
  asio::io_context io;
  auto tools = std::make_shared<ToolRegistry>();
  auto spec = MakeSpec(LoadWorkflow(*tools), tools, io);

  // A default IS available — the point is that it must NOT be used. Silently
  // demoting an agent from its intended cloud model to a local one would
  // change answer quality invisibly (design spec §5).
  spec.backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  spec.agent_name = "cloud";  // wants "cloud-big", which is not registered

  EXPECT_THROW(BuildAgentNode(spec), AgentflowError);
}

}  // namespace
}  // namespace agentflow::workflow
