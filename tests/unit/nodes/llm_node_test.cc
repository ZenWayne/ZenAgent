// tests/unit/nodes/llm_node_test.cc
//
// Ctor-validation only in this file. A real-LLM smoke would need MODEL_PATH
// and is `manual` like agent_node_test; we follow that convention so a
// default `bazel test //tests/unit/nodes/...` doesn't try to load Gemma.

#include "agentflow/nodes/llm_node.h"

#include <gtest/gtest.h>

#include <asio/io_context.hpp>

#include "agentflow/core/errors.h"

namespace agentflow {
namespace {

TEST(LlmNodeTest, CtorRejectsMissingId) {
  LlmNodeConfig cfg;
  cfg.engine =
      std::shared_ptr<LiteRtLmEngine>(nullptr);  // checked AFTER id
  // engine still null here, but id-check fires first.
  asio::io_context io;
  cfg.io_ctx = &io;
  EXPECT_THROW(LlmNode n(std::move(cfg)), AgentflowError);
}

TEST(LlmNodeTest, CtorRejectsMissingEngine) {
  LlmNodeConfig cfg;
  cfg.id = "llm";
  asio::io_context io;
  cfg.io_ctx = &io;
  EXPECT_THROW(LlmNode n(std::move(cfg)), AgentflowError);
}

TEST(LlmNodeTest, CtorRejectsMissingIoCtx) {
  LlmNodeConfig cfg;
  cfg.id = "llm";
  // Fake an engine shared_ptr from a raw pointer literal — the ctor only
  // checks for null, never touches the engine before throwing on io_ctx.
  cfg.engine.reset(reinterpret_cast<LiteRtLmEngine*>(0x1),
                   [](LiteRtLmEngine*) {});
  EXPECT_THROW(LlmNode n(std::move(cfg)), AgentflowError);
}

}  // namespace
}  // namespace agentflow
