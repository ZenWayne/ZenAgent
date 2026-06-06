#include "agentflow/workflow/workflow.h"

#include <utility>

#include "agentflow/core/state.h"

namespace agentflow::workflow {

Workflow::Workflow(proto::WorkflowSpec spec) : spec_(std::move(spec)) {}

::agentflow::State Workflow::NewEmptyState() const {
  const auto& st = spec_.state();
  if (st.kind() == "dynamic_json") {
    nlohmann::ordered_json fields;
    for (const auto& [name, decl] : st.fields()) {
      nlohmann::ordered_json f = nlohmann::ordered_json::object();
      f["type"] = decl.type();
      if (!decl.default_value_json().empty()) {
        auto parsed = nlohmann::ordered_json::parse(decl.default_value_json(),
                                                       nullptr, false);
        if (!parsed.is_discarded()) f["default"] = parsed;
      }
      fields[name] = f;
    }
    return ::agentflow::State::FromJson(fields);
  }
  if (st.kind() == "proto_dynamic" && state_pool_) {
    return ::agentflow::State::FromDynamicProto(state_pool_, st.message_type());
  }
  return {};
}

}  // namespace agentflow::workflow
