#ifndef AGENTFLOW_WORKFLOW_WORKFLOW_H_
#define AGENTFLOW_WORKFLOW_WORKFLOW_H_

#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "workflow_spec.pb.h"

namespace google {
namespace protobuf {
class DescriptorPool;
}  // namespace protobuf
}  // namespace google

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
  // tier 1 (proto)         — default-constructed State (no message yet).
  // tier 2 (dynamic_json)  — State::FromJson(fields).
  // tier 3 (proto_dynamic) — State::FromDynamicProto(pool, message_type)
  //                          provided a pool was wired via SetStatePool().
  ::agentflow::State NewEmptyState() const;

  // Wires a descriptor pool used by tier-3 (proto_dynamic) state. The
  // loader builds the pool from the spec's descriptor_set_path /
  // descriptor_set_b64 and installs it here. Lifetime: held via shared_ptr
  // for as long as the Workflow (and any State derived from NewEmptyState())
  // is alive.
  void SetStatePool(std::shared_ptr<google::protobuf::DescriptorPool> pool) {
    state_pool_ = std::move(pool);
  }

 private:
  proto::WorkflowSpec spec_;
  std::shared_ptr<google::protobuf::DescriptorPool> state_pool_;
};

}  // namespace agentflow::workflow

#endif
