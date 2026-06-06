#include "agentflow/workflow/workflow.h"

#include <utility>

#include "agentflow/core/state.h"

namespace agentflow::workflow {

Workflow::Workflow(proto::WorkflowSpec spec) : spec_(std::move(spec)) {}

::agentflow::State Workflow::NewEmptyState() const {
  if (spec_.state().kind() == "dynamic_json") {
    nlohmann::ordered_json fields;
    for (const auto& [name, decl] : spec_.state().fields()) {
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
  // tier 2/3 land in P14+.
  return {};
}

}  // namespace agentflow::workflow
