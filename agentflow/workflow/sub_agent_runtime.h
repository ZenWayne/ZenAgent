#ifndef AGENTFLOW_WORKFLOW_SUB_AGENT_RUNTIME_H_
#define AGENTFLOW_WORKFLOW_SUB_AGENT_RUNTIME_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/event.h"
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
  // One conversation turn: send a message JSON, get the model's full
  // response JSON back. This is the only LLM operation RunSync performs, so
  // it is the entire injected surface — see ConversationFactory.
  using SendFn =
      std::function<absl::StatusOr<std::string>(const std::string& message_json)>;

  // Builds a conversation for a child agent from the prepared options and
  // returns its bound SendFn. An empty (falsy) SendFn means the conversation
  // could not be created and is treated as engine_error by RunSync.
  //
  // Production passes DefaultConversationFactory (real LiteRT-LM engine).
  // Tests inject a fake to drive RunSync without a model — same dependency-
  // injection seam as ClientFactory in McpClientPool. There is no test-only
  // branch inside RunSync.
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

  // Synchronous sub-agent run. Returns a JSON value (typically a string,
  // or an error object {"error":"<kind>",...}). NEVER throws.
  [[nodiscard]] nlohmann::ordered_json RunSync(std::string_view parent_agent,
                                                  std::string_view child_agent,
                                                  std::string_view goal,
                                                  const SubAgentContext& ctx);

 private:
  std::shared_ptr<Workflow> wf_;
  const ToolRegistry& host_tools_;
  EventEmitter& emit_;
  ConversationFactory conv_factory_;
  ::asio::io_context* io_ = nullptr;
};

}  // namespace agentflow::workflow
#endif
