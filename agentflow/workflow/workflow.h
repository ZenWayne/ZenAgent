#ifndef AGENTFLOW_WORKFLOW_WORKFLOW_H_
#define AGENTFLOW_WORKFLOW_WORKFLOW_H_

#include <string>

#include <nlohmann/json.hpp>

#include "workflow_spec.pb.h"

namespace agentflow {
class State;
}

namespace agentflow::workflow {

// Compiled workflow: validated spec + materialized resources.
// Does NOT own a Graph — graph materialization happens at Run time.
class Workflow {
 public:
  explicit Workflow(proto::WorkflowSpec spec);

  const std::string& name()    const { return spec_.name(); }
  const std::string& version() const { return spec_.version(); }
  const proto::WorkflowSpec& spec() const { return spec_; }

  // Emit a fresh empty State matching this workflow's state.kind.
  // Phase 1/2/3 ship tier 1 only; tier 2/3 land later. For unsupported
  // tiers, returns a default-constructed State.
  ::agentflow::State NewEmptyState() const;

 private:
  proto::WorkflowSpec spec_;
};

}  // namespace agentflow::workflow

#endif
