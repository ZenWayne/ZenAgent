// tests/unit/core/errors_test.cc
#include "agentflow/core/errors.h"

#include <gtest/gtest.h>

namespace agentflow {
namespace {

TEST(ErrorsTest, BaseHierarchy) {
  try {
    throw LlmAbortedError("user pressed back");
  } catch (const LlmError& e) {
    EXPECT_STREQ(e.what(), "user pressed back");
    SUCCEED();
    return;
  }
  FAIL() << "should have caught LlmError";
}

TEST(ErrorsTest, AllRootDerivedFromAgentflowError) {
  try {
    throw GraphCompileError("bad cycle");
  } catch (const AgentflowError& e) {
    EXPECT_NE(std::string(e.what()).find("bad cycle"), std::string::npos);
    return;
  }
  FAIL();
}

TEST(ErrorsTest, ToolErrorIsAgentflowError) {
  try {
    throw McpTransportError("connection refused");
  } catch (const ToolError&) {
    SUCCEED();
    return;
  }
  FAIL();
}

}  // namespace
}  // namespace agentflow
