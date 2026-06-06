#include "agentflow/workflow/sub_agent_runtime.h"

#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <absl/strings/str_cat.h>
#include <asio/io_context.hpp>

#include "agentflow/inference/litert_lm_conversation.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/workflow/json_path.h"
#include "agentflow/workflow/template_engine.h"
#include "workflow_spec.pb.h"

namespace agentflow::workflow {
namespace {

std::string GenUuidLike() {
  std::random_device rd;
  std::mt19937_64 rng(rd());
  std::ostringstream s;
  s << std::hex << rng() << "-" << rng();
  return s.str();
}

const proto::WorkflowSpec::AgentDef* FindAgent(const Workflow& wf,
                                                  std::string_view name) {
  auto it = wf.spec().agents().find(std::string(name));
  if (it == wf.spec().agents().end()) return nullptr;
  return &it->second;
}

}  // namespace

SubAgentRuntime::SubAgentRuntime(std::shared_ptr<Workflow> wf,
                                    const ToolRegistry& host_tools,
                                    EventEmitter& emit)
    : wf_(std::move(wf)), host_tools_(host_tools), emit_(emit) {}

SubAgentRuntime::SubAgentRuntime(
    std::shared_ptr<Workflow> wf, const ToolRegistry& host_tools,
    EventEmitter& emit,
    std::shared_ptr<::agentflow::LiteRtLmEngine> engine,
    ::asio::io_context& io)
    : wf_(std::move(wf)),
      host_tools_(host_tools),
      emit_(emit),
      engine_(std::move(engine)),
      io_(&io) {}

nlohmann::ordered_json SubAgentRuntime::RunSync(
    std::string_view parent_agent, std::string_view child_agent,
    std::string_view goal, const SubAgentContext& ctx) {
  std::string invocation_id = GenUuidLike();
  std::string root_id =
      ctx.depth == 0 ? invocation_id : ctx.root_invocation_id;

  const auto* parent_def = FindAgent(*wf_, parent_agent);
  if (!parent_def || !parent_def->has_delegates()) {
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "unknown_agent", 0);
    return nlohmann::ordered_json{{"error", "unknown_agent"}};
  }
  if (ctx.depth >= parent_def->delegates().max_depth()) {
    emit_.EmitSubAgentStart(parent_agent, child_agent, invocation_id, root_id,
                              ctx.depth, goal);
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false,
                           "max_depth_exceeded", 0);
    return nlohmann::ordered_json{
        {"error", "max_depth_exceeded"},
        {"depth", static_cast<uint32_t>(ctx.depth)}};
  }
  // Child must be in roster.
  bool in_roster = false;
  for (const auto& a : parent_def->delegates().agents()) {
    if (a == child_agent) {
      in_roster = true;
      break;
    }
  }
  if (!in_roster) {
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "unknown_agent", 0);
    return nlohmann::ordered_json{{"error", "unknown_agent"}};
  }

  emit_.EmitSubAgentStart(parent_agent, child_agent, invocation_id, root_id,
                            ctx.depth, goal);

  const auto& child = wf_->spec().agents().at(std::string(child_agent));

  // No engine = skeleton mode (hermetic tests). Return a stub success.
  if (!engine_ || !io_) {
    std::string result = absl::StrCat("[stub:", child_agent, "] ", goal);
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, true, "",
                            static_cast<uint32_t>(result.size()));
    return nlohmann::ordered_json(result);
  }

  // ── Real-LLM path ───────────────────────────────────────────────────────
  // Build system message + per-child tool slice + send the goal as the
  // initial user message. This PR wires a single-turn conversation; the
  // multi-turn tool dispatch loop is left for a follow-up — the conversation
  // already carries history server-side so additional turns can be added
  // incrementally without API changes here.
  EvalContext sys_ctx;
  sys_ctx.workflow_name = wf_->name();
  sys_ctx.workflow_version = wf_->version();
  std::string system_text;
  if (!child.system_prompt().empty()) {
    auto tmpl_or = TemplateString::Parse(child.system_prompt());
    if (!tmpl_or.ok()) {
      emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "bad_template",
                              0);
      return nlohmann::ordered_json{{"error", "bad_template"}};
    }
    auto v = tmpl_or->Evaluate(sys_ctx);
    system_text = v.is_string() ? v.get<std::string>() : v.dump();
  }

  // Per-child tool slicing (Task 4.6).
  std::vector<std::string> child_tools;
  child_tools.reserve(child.tools_size());
  for (const auto& t : child.tools()) child_tools.push_back(t);
  std::string tools_json = host_tools_.ExportToolsJson(
      std::span<const std::string>(child_tools));
  if (tools_json.empty()) tools_json = "[]";

  // Build the LiteRtLmConversation. system_message_json must be the full
  // {"role":"system","content":"..."} JSON object — that's what the engine
  // feeds the chat template.
  LiteRtLmConversationOptions opts;
  if (!system_text.empty()) {
    nlohmann::ordered_json sys_msg = {{"role", "system"},
                                        {"content", system_text}};
    opts.system_message_json = sys_msg.dump();
  }
  opts.tools_json = tools_json;
  opts.constrained_tool_calls = child.model().constrained_tool_calls();
  if (child.model().max_output_tokens() > 0) {
    opts.max_output_tokens = child.model().max_output_tokens();
  } else {
    opts.max_output_tokens = 512;
  }

  auto conv = LiteRtLmConversation::Create(engine_, std::move(opts), *io_);
  if (!conv) {
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "engine_error", 0);
    return nlohmann::ordered_json{{"error", "engine_error"}};
  }

  nlohmann::ordered_json user_msg = {
      {"role", "user"},
      {"content", nlohmann::ordered_json::array(
                       {{{"type", "text"}, {"text", std::string(goal)}}})}};
  auto resp_or = conv->SendMessageSync(user_msg.dump());
  if (!resp_or.ok()) {
    emit_.EmitSubAgentEnd(invocation_id, ctx.depth, false, "engine_error", 0);
    return nlohmann::ordered_json{{"error", "engine_error"}};
  }
  std::string raw_response = std::move(*resp_or);

  // Output extraction.
  std::string extract = parent_def->delegates().output_extract().empty()
                            ? "$.content[0].text"
                            : parent_def->delegates().output_extract();
  auto path_or = JsonPath::Parse(extract);
  std::string final_str;
  if (path_or.ok()) {
    auto root = nlohmann::ordered_json::parse(raw_response, nullptr, false);
    if (!root.is_discarded()) {
      auto val = path_or->Resolve(root);
      if (val && val->is_string()) {
        final_str = val->get<std::string>();
      } else {
        emit_.EmitSubAgentExtractFailed(invocation_id, extract);
        final_str = raw_response;
      }
    } else {
      final_str = raw_response;
    }
  } else {
    final_str = raw_response;
  }

  emit_.EmitSubAgentEnd(invocation_id, ctx.depth, true, "",
                          static_cast<uint32_t>(final_str.size()));
  return nlohmann::ordered_json(final_str);
}

}  // namespace agentflow::workflow
