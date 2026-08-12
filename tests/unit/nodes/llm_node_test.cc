// tests/unit/nodes/llm_node_test.cc
//
// Ctor-validation only in this file. A real-LLM smoke would need MODEL_PATH
// and is `manual` like agent_node_test; we follow that convention so a
// default `bazel test //tests/unit/nodes/...` doesn't try to load Gemma.

#include "agentflow/nodes/llm_node.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/errors.h"
#include "agentflow/observability/callback_event_emitter.h"
#include "test_messages.pb.h"
#include "tests/support/fake_chat_backend.h"

namespace agentflow {
namespace {

struct EventCapture {
  std::vector<proto::TraceEvent> events;
  std::mutex m;
  CallbackEventEmitter emitter{[this](const proto::TraceEvent& e) {
    std::lock_guard<std::mutex> l(m);
    events.push_back(e);
  }};

  std::vector<std::string> tokens() {
    std::lock_guard<std::mutex> l(m);
    std::vector<std::string> out;
    for (const auto& e : events) {
      if (e.has_token()) out.push_back(e.token().token());
    }
    return out;
  }
};

TEST(LlmNodeTest, CtorRejectsMissingId) {
  LlmNodeConfig cfg;
  cfg.backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  // backend is set, but id-check fires first.
  asio::io_context io;
  cfg.io_ctx = &io;
  EXPECT_THROW(LlmNode n(std::move(cfg)), AgentflowError);
}

TEST(LlmNodeTest, CtorRejectsMissingBackend) {
  LlmNodeConfig cfg;
  cfg.id = "llm";
  asio::io_context io;
  cfg.io_ctx = &io;
  // cfg.backend deliberately left null.
  EXPECT_THROW(LlmNode n(std::move(cfg)), AgentflowError);
}

TEST(LlmNodeTest, CtorRejectsMissingIoCtx) {
  LlmNodeConfig cfg;
  cfg.id = "llm";
  cfg.backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{});
  EXPECT_THROW(LlmNode n(std::move(cfg)), AgentflowError);
}

TEST(LlmNodeTest, WritesAssistantTextToOutputField) {
  asio::io_context io;
  auto backend = std::make_shared<testing::FakeChatBackend>(
      std::vector<std::string>{
          R"({"role":"assistant","content":[{"type":"text","text":"pong"}]})"});

  LlmNodeConfig cfg;
  cfg.id = "llm";
  cfg.io_ctx = &io;
  cfg.backend = backend;
  cfg.input_field = "user_query";
  cfg.output_field = "assistant_reply";
  LlmNode node(std::move(cfg));

  test::TestState raw;
  raw.set_user_query("ping");
  CancelSource cancel;
  EventCapture cap;  // same helper struct as in agent_node_test.cc

  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        co_return co_await node.Run(State::From(std::move(raw)),
                                     cancel.Token(), cap.emitter);
      },
      asio::use_future);
  io.run();

  EXPECT_EQ(fut.get().As<test::TestState>().assistant_reply(), "pong");
}

}  // namespace
}  // namespace agentflow
