// tests/unit/nodes/agent_node_test.cc
#include "agentflow/nodes/agent_node.h"

#include <gtest/gtest.h>

#include "test_messages.pb.h"

namespace agentflow {
namespace {

// AgentNode test requires a real LiteRT-LM model. Marked manual.
TEST(AgentNodeTest, DISABLED_SimpleResponse) {
  const char* model_path = std::getenv("MODEL_PATH");
  ASSERT_NE(model_path, nullptr);

  auto engine = LiteRtLmEngine::Create(
      LiteRtLmEngineOptions{.model_path = model_path});

  asio::io_context io;
  AgentNodeConfig cfg;
  cfg.engine = engine;
  cfg.io_ctx = &io;
  cfg.system_prompt = "You are a helpful assistant. Reply briefly.";
  cfg.input_field = "user_query";
  cfg.output_field = "assistant_reply";

  auto node = std::make_unique<AgentNode>(std::move(cfg));

  test::TestState raw;
  raw.set_user_query("Say hello in one word");

  CancelSource cancel;

  auto fut = asio::co_spawn(io,
      [&]() -> asio::awaitable<State> {
        NullEventEmitter null_emit;
        State state = State::From(std::move(raw));
        co_return co_await node->Run(
            std::move(state), cancel.Token(), null_emit);
      },
      asio::use_future);
  io.run();
  auto out = fut.get();

  std::string reply = out.As<test::TestState>().assistant_reply();
  EXPECT_FALSE(reply.empty());
  std::cout << "Agent reply: " << reply << std::endl;
}

}  // namespace
}  // namespace agentflow
