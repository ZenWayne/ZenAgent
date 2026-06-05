#include "agentflow/workflow/workflow_loader.h"

#include <gtest/gtest.h>
#include <absl/strings/match.h>
#include <asio/io_context.hpp>

#include "agentflow/tools/tool_registry.h"

namespace agentflow::workflow {
namespace {

constexpr char kMinimalJson[] = R"({
  "schema_version": 1,
  "name": "test_wf",
  "version": "v1",
  "state": {
    "kind": "dynamic_json",
    "fields": { "user_query": {"type":"string"} }
  },
  "agents": {
    "chat": {
      "system_prompt": "be brief",
      "model": {"max_output_tokens": 128, "constrained_tool_calls": false},
      "tools": []
    }
  },
  "main": "chat"
})";

TEST(WorkflowLoaderTest, LoadMinimalJson) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(kMinimalJson, host_tools);
  ASSERT_TRUE(wf_or.ok()) << wf_or.status();
  EXPECT_EQ((*wf_or)->name(), "test_wf");
  EXPECT_EQ((*wf_or)->version(), "v1");
}

TEST(WorkflowLoaderTest, RejectMissingMain) {
  std::string bad = R"({
    "schema_version": 1,
    "name": "x", "version": "v1",
    "state": {"kind":"dynamic_json","fields":{}},
    "agents": {"a": {"system_prompt":"", "model":{}, "tools":[]}},
    "main": "ghost"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(bad, host_tools);
  EXPECT_FALSE(wf_or.ok());
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "main") ||
              absl::StrContains(wf_or.status().message(), "ghost"));
}

TEST(WorkflowLoaderTest, RejectUnknownToolReference) {
  std::string bad = R"({
    "schema_version": 1,
    "name":"x","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{"a":{"system_prompt":"","model":{},"tools":["does_not_exist"]}},
    "main":"a"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(bad, host_tools);
  EXPECT_FALSE(wf_or.ok());
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "does_not_exist"));
}

TEST(WorkflowLoaderTest, RejectFutureSchemaVersion) {
  std::string bad = R"({
    "schema_version": 999,
    "name":"x","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{"a":{"system_prompt":"","model":{},"tools":[]}},
    "main":"a"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  EXPECT_FALSE(WorkflowLoader::Load(bad, host_tools).ok());
}

TEST(WorkflowLoaderTest, RejectMalformedJson) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  EXPECT_FALSE(WorkflowLoader::Load("{not json", host_tools).ok());
}

TEST(WorkflowLoaderTest, RejectTemplateReferencingUnknownStateField) {
  std::string bad = R"({
    "schema_version": 1,
    "name":"x","version":"v1",
    "state":{"kind":"dynamic_json","fields":{"only_field":{"type":"string"}}},
    "agents":{
      "a":{"system_prompt":"","model":{},"tools":[],
           "delegates":{"agents":["b"],"max_depth":2,
                         "input_template":{"u":"{{state.does_not_exist}}"}}},
      "b":{"system_prompt":"","model":{},"tools":[]}
    },
    "main":"a"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(bad, host_tools);
  EXPECT_FALSE(wf_or.ok());
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "does_not_exist"));
}

}  // namespace
}  // namespace agentflow::workflow
