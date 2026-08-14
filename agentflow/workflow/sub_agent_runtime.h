#ifndef AGENTFLOW_WORKFLOW_SUB_AGENT_RUNTIME_H_
#define AGENTFLOW_WORKFLOW_SUB_AGENT_RUNTIME_H_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include "agentflow/core/event.h"
#include "agentflow/core/token_channel.h"
#include "agentflow/inference/chat_backend.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/sub_agent_context.h"
#include "agentflow/workflow/workflow.h"

namespace agentflow::workflow {

// Resolves a logical backend name against a default backend and a map of
// named backends the host registered. An empty `backend_name` selects
// `default_backend`; a non-empty name that is not in `backends` THROWS
// AgentflowError rather than silently falling back to the default —
// silently demoting an agent from its intended cloud model to a local one
// would change answer quality invisibly (design spec §5). `requesting_agent`
// is only used to make the error message actionable.
//
// Shared by top-level agent backend selection (workflow_runner.cc) and
// sub-agent backend selection (SubAgentRuntime::RunAsync) so both follow the
// identical rule instead of maintaining two copies of it.
std::shared_ptr<::agentflow::IChatBackend> ResolveNamedBackend(
    std::string_view backend_name, std::string_view requesting_agent,
    const std::shared_ptr<::agentflow::IChatBackend>& default_backend,
    const std::map<std::string, std::shared_ptr<::agentflow::IChatBackend>>&
        backends);

class SubAgentRuntime {
 public:
  // Called once per streamed text delta during a turn (only on the streaming,
  // i.e. unconstrained, path). Empty/unset means "don't stream".
  //
  // One definition lives in agentflow/inference/chat_backend.h. The signature
  // is unchanged, so every existing caller compiles as before.
  using TokenSink = ::agentflow::TokenSink;

  // One conversation turn: send a message JSON, await the model's full response
  // JSON. Async so RunAsync co_awaits it under the caller's io_context without
  // driving the loop itself. When `on_token` is set and the conversation is
  // unconstrained, each text delta is delivered to it as it streams; the
  // returned string is always the full (canonical) response JSON. `cancel`
  // lets the conversation register its in-flight cancel hook so the sub-agent
  // stops mid-decode (not just at turn boundaries). This is the only LLM
  // operation RunAsync performs — the entire injected surface.
  using SendFn = std::function<asio::awaitable<absl::StatusOr<std::string>>(
      const std::string& message_json, const TokenSink& on_token,
      const ::agentflow::CancelToken& cancel)>;

  // Builds a conversation for a child agent from the prepared options and
  // returns its bound SendFn. `backend_name` is the child's own
  // ModelSpec.backend (empty means "use the host default"); `requesting_agent`
  // is the child agent's name, used only for error messages. An empty
  // (falsy) SendFn means the conversation could not be created and is
  // treated as engine_error by RunAsync.
  //
  // Production passes DefaultConversationFactory (real chat backend(s)).
  // Tests inject a fake to drive RunAsync without a model — same dependency-
  // injection seam as ClientFactory in McpClientPool. There is no test-only
  // branch inside RunAsync.
  using ConversationFactory = std::function<SendFn(
      std::string_view backend_name, std::string_view requesting_agent,
      ::agentflow::ChatConversationOptions)>;

  SubAgentRuntime(std::shared_ptr<Workflow> wf,
                   const ToolRegistry& host_tools,
                   EventEmitter& emit,
                   ConversationFactory conv_factory);

  // Builds the production factory from the host's default backend plus any
  // named backends it registered. A child whose ModelSpec.backend is empty
  // gets `default_backend`; a child that names a backend gets THAT backend
  // (or the factory call throws — see ResolveNamedBackend) rather than
  // silently inheriting the parent's backend (Task fix: sub-agents used to
  // ignore their own model.backend entirely).
  static ConversationFactory DefaultConversationFactory(
      std::shared_ptr<::agentflow::IChatBackend> default_backend,
      std::map<std::string, std::shared_ptr<::agentflow::IChatBackend>>
          backends = {});

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
};

}  // namespace agentflow::workflow
#endif
