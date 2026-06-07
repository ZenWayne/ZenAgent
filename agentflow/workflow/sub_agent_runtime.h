#ifndef AGENTFLOW_WORKFLOW_SUB_AGENT_RUNTIME_H_
#define AGENTFLOW_WORKFLOW_SUB_AGENT_RUNTIME_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include "agentflow/core/event.h"
#include "agentflow/core/token_channel.h"
#include "agentflow/inference/litert_lm_conversation.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/sub_agent_context.h"
#include "agentflow/workflow/workflow.h"

namespace agentflow {
class LiteRtLmEngine;
}

namespace asio { class io_context; }

namespace agentflow::workflow {

class SubAgentRuntime {
 public:
  // Called once per streamed text delta during a turn (only on the streaming,
  // i.e. unconstrained, path). Empty/unset means "don't stream".
  using TokenSink = std::function<void(std::string_view delta)>;

  // One conversation turn: send a message JSON, await the model's full response
  // JSON. Async so RunAsync co_awaits it under the caller's io_context without
  // driving the loop itself. When `on_token` is set and the conversation is
  // unconstrained, each text delta is delivered to it as it streams; the
  // returned string is always the full (canonical) response JSON. This is the
  // only LLM operation RunAsync performs — the entire injected surface.
  using SendFn = std::function<asio::awaitable<absl::StatusOr<std::string>>(
      const std::string& message_json, const TokenSink& on_token)>;

  // Builds a conversation for a child agent from the prepared options and
  // returns its bound SendFn. An empty (falsy) SendFn means the conversation
  // could not be created and is treated as engine_error by RunAsync.
  //
  // Production passes DefaultConversationFactory (real LiteRT-LM engine).
  // Tests inject a fake to drive RunAsync without a model — same dependency-
  // injection seam as ClientFactory in McpClientPool. There is no test-only
  // branch inside RunAsync.
  using ConversationFactory = std::function<SendFn(LiteRtLmConversationOptions)>;

  SubAgentRuntime(std::shared_ptr<Workflow> wf,
                   const ToolRegistry& host_tools,
                   EventEmitter& emit,
                   ConversationFactory conv_factory,
                   ::asio::io_context& io);

  // Builds the production factory backed by a real LiteRT-LM engine.
  static ConversationFactory DefaultConversationFactory(
      std::shared_ptr<::agentflow::LiteRtLmEngine> engine,
      ::asio::io_context& io);

  // Async sub-agent run. Returns a JSON value (typically a string, or an error
  // object {"error":"<kind>",...}). NEVER throws. Runs entirely under the
  // caller's io_context via co_await (no nested io.run()), so it composes with
  // a concurrent token-channel drain at the top of the stack. When
  // ctx.token_channel is set and the child is unconstrained, each generated
  // text delta is pushed onto that channel as it streams.
  [[nodiscard]] asio::awaitable<nlohmann::ordered_json> RunAsync(
      std::string_view parent_agent, std::string_view child_agent,
      std::string_view goal, SubAgentContext ctx);

 private:
  std::shared_ptr<Workflow> wf_;
  const ToolRegistry& host_tools_;
  EventEmitter& emit_;
  ConversationFactory conv_factory_;
  // Kept for construction symmetry; RunAsync no longer drives the loop itself
  // (it co_awaits under the caller's io_context), so this is currently unused.
  [[maybe_unused]] ::asio::io_context* io_ = nullptr;
};

}  // namespace agentflow::workflow
#endif
