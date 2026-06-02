// agentflow/nodes/router_node.cc
#include "agentflow/nodes/router_node.h"

#include <utility>

#include "agentflow/core/errors.h"

namespace agentflow {

namespace {

void WriteStringField(State& s, const std::string& f, const std::string& v) {
  if (f.empty()) return;
  auto* msg = const_cast<google::protobuf::Message*>(s.UnsafeMessage());
  if (!msg) return;
  const auto* refl = msg->GetReflection();
  const auto* desc = msg->GetDescriptor()->FindFieldByName(f);
  if (!desc) return;
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    refl->SetString(msg, desc, v);
  }
}

}  // namespace

RouterNode::RouterNode(RouterNodeConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.id.empty()) throw AgentflowError("RouterNode: id required");
  if (!cfg_.chooser) throw AgentflowError("RouterNode: chooser fn required");
}

asio::awaitable<State> RouterNode::Run(State state, const CancelToken& cancel,
                                        EventEmitter& /*emit*/) {
  if (cancel.IsCancelled()) co_return std::move(state);
  std::string choice = cfg_.chooser(state);
  WriteStringField(state, cfg_.output_field, choice);
  co_return std::move(state);
}

}  // namespace agentflow
