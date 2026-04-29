// agentflow/core/event.cc
#include "agentflow/core/event.h"

#include <chrono>

namespace agentflow {

int64_t EventEmitter::NowMicros() {
  using namespace std::chrono;
  return duration_cast<microseconds>(
             system_clock::now().time_since_epoch())
      .count();
}

void EventEmitter::EmitToken(std::string_view node_id,
                             std::string_view token) {
  proto::TraceEvent ev;
  ev.set_kind(proto::TraceEvent::TOKEN);
  ev.set_node_id(std::string(node_id));
  ev.set_unix_micros(NowMicros());
  ev.mutable_token()->set_token(std::string(token));
  Emit(std::move(ev));
}

void EventEmitter::EmitNodeStart(std::string_view node_id) {
  proto::TraceEvent ev;
  ev.set_kind(proto::TraceEvent::NODE_START);
  ev.set_node_id(std::string(node_id));
  ev.set_unix_micros(NowMicros());
  ev.mutable_node_start();
  Emit(std::move(ev));
}

void EventEmitter::EmitNodeEnd(std::string_view node_id, bool cancelled,
                               bool failed) {
  proto::TraceEvent ev;
  ev.set_kind(proto::TraceEvent::NODE_END);
  ev.set_node_id(std::string(node_id));
  ev.set_unix_micros(NowMicros());
  auto* p = ev.mutable_node_end();
  p->set_cancelled(cancelled);
  p->set_failed(failed);
  Emit(std::move(ev));
}

void EventEmitter::EmitToolCall(std::string_view node_id,
                                std::string_view tool_name,
                                std::string_view args_json) {
  proto::TraceEvent ev;
  ev.set_kind(proto::TraceEvent::TOOL_CALL);
  ev.set_node_id(std::string(node_id));
  ev.set_unix_micros(NowMicros());
  auto* p = ev.mutable_tool_call();
  p->set_tool_name(std::string(tool_name));
  p->set_args_json(std::string(args_json));
  Emit(std::move(ev));
}

void EventEmitter::EmitToolReturn(std::string_view node_id,
                                  std::string_view tool_name,
                                  std::string_view result_json) {
  proto::TraceEvent ev;
  ev.set_kind(proto::TraceEvent::TOOL_RETURN);
  ev.set_node_id(std::string(node_id));
  ev.set_unix_micros(NowMicros());
  auto* p = ev.mutable_tool_return();
  p->set_tool_name(std::string(tool_name));
  p->set_result_json(std::string(result_json));
  Emit(std::move(ev));
}

void EventEmitter::EmitEdgeFire(std::string_view from, std::string_view to,
                                int group) {
  proto::TraceEvent ev;
  ev.set_kind(proto::TraceEvent::EDGE_FIRE);
  ev.set_unix_micros(NowMicros());
  auto* p = ev.mutable_edge_fire();
  p->set_from_node(std::string(from));
  p->set_to_node(std::string(to));
  p->set_activation_group(group);
  Emit(std::move(ev));
}

void EventEmitter::EmitNodeFailed(std::string_view node_id,
                                  std::string_view type,
                                  std::string_view message) {
  proto::TraceEvent ev;
  ev.set_kind(proto::TraceEvent::NODE_FAILED);
  ev.set_node_id(std::string(node_id));
  ev.set_unix_micros(NowMicros());
  auto* p = ev.mutable_failure();
  p->set_node_id(std::string(node_id));
  p->set_type(std::string(type));
  p->set_message(std::string(message));
  Emit(std::move(ev));
}

void EventEmitter::EmitGraphDone(bool failed) {
  proto::TraceEvent ev;
  ev.set_kind(proto::TraceEvent::GRAPH_DONE);
  ev.set_unix_micros(NowMicros());
  auto* p = ev.mutable_node_end();
  p->set_failed(failed);
  Emit(std::move(ev));
}

}  // namespace agentflow
