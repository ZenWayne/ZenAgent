#include "agentflow/workflow/template_engine.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/state.h"
#include "agentflow/workflow/eval_context.h"

namespace agentflow::workflow {
namespace {

EvalContext CtxWithState(const State* s) {
  EvalContext c;
  c.state = s;
  c.workflow_name = "test_wf";
  c.workflow_version = "v1";
  return c;
}

TEST(TemplateEngineTest, PureSubstitutionStringRoundTrip) {
  auto tmpl_or = TemplateString::Parse("{{state.user_query}}");
  ASSERT_TRUE(tmpl_or.ok()) << tmpl_or.status();
  nlohmann::ordered_json fields;
  fields["user_query"] = nlohmann::ordered_json{{"type", "string"}};
  State s = State::FromJson(fields);
  WriteStringField(s, "user_query", "hello");
  auto ctx = CtxWithState(&s);
  auto val = tmpl_or->Evaluate(ctx);
  ASSERT_TRUE(val.is_string());
  EXPECT_EQ(val.get<std::string>(), "hello");
}

TEST(TemplateEngineTest, StringInterpolation) {
  auto tmpl_or = TemplateString::Parse("Hello {{state.name}}!");
  ASSERT_TRUE(tmpl_or.ok());
  nlohmann::ordered_json fields;
  fields["name"] = nlohmann::ordered_json{{"type", "string"}};
  State s = State::FromJson(fields);
  WriteStringField(s, "name", "world");
  auto ctx = CtxWithState(&s);
  auto val = tmpl_or->Evaluate(ctx);
  ASSERT_TRUE(val.is_string());
  EXPECT_EQ(val.get<std::string>(), "Hello world!");
}

TEST(TemplateEngineTest, WorkflowMetadataPath) {
  auto tmpl_or = TemplateString::Parse("Workflow: {{workflow.name}}");
  ASSERT_TRUE(tmpl_or.ok());
  EvalContext ctx = CtxWithState(nullptr);
  auto val = tmpl_or->Evaluate(ctx);
  EXPECT_EQ(val.get<std::string>(), "Workflow: test_wf");
}

TEST(TemplateEngineTest, ToolArgsPath) {
  auto tmpl_or = TemplateString::Parse("{{tool.goal}}");
  ASSERT_TRUE(tmpl_or.ok());
  nlohmann::ordered_json args = {{"goal", "do thing"}};
  EvalContext ctx;
  ctx.tool_args = &args;
  auto val = tmpl_or->Evaluate(ctx);
  EXPECT_EQ(val.get<std::string>(), "do thing");
}

TEST(TemplateEngineTest, ParseErrorOnUnbalancedBraces) {
  EXPECT_FALSE(TemplateString::Parse("{{state.x").ok());
  EXPECT_FALSE(TemplateString::Parse("state.x}}").ok());
}

TEST(TemplateEngineTest, UnknownPathHeadIsParseError) {
  EXPECT_FALSE(TemplateString::Parse("{{garbage.x}}").ok());
}

}  // namespace
}  // namespace agentflow::workflow
