// agentflow/core/event.h
#ifndef AGENTFLOW_CORE_EVENT_H_
#define AGENTFLOW_CORE_EVENT_H_

#include <chrono>
#include <cstdint>
#include <string_view>

#include "trace_event.pb.h"

namespace agentflow {

class EventEmitter {
 public:
  virtual ~EventEmitter() = default;

  // Lowest-level API: subclasses override this to actually deliver the event.
  virtual void Emit(proto::TraceEvent ev) = 0;

  // Convenience wrappers — implemented in event.cc on top of Emit.
  void EmitToken(std::string_view node_id, std::string_view token);
  void EmitNodeStart(std::string_view node_id);
  void EmitNodeEnd(std::string_view node_id, bool cancelled, bool failed);
  void EmitToolCall(std::string_view node_id, std::string_view tool_name,
                    std::string_view args_json);
  void EmitToolReturn(std::string_view node_id, std::string_view tool_name,
                      std::string_view result_json);
  void EmitEdgeFire(std::string_view from, std::string_view to, int group);
  void EmitNodeFailed(std::string_view node_id, std::string_view type,
                       std::string_view message);
  void EmitGraphDone(bool failed);
  void EmitWorkflowRegistered(std::string_view name, std::string_view version,
                                bool signed_value, std::string_view key_id);
  void EmitWorkflowUnregistered(std::string_view name, std::string_view version);
  void EmitSubAgentStart(std::string_view parent_agent,
                          std::string_view child_agent,
                          std::string_view invocation_id,
                          std::string_view root_invocation_id,
                          uint32_t depth,
                          std::string_view goal);
  void EmitSubAgentEnd(std::string_view invocation_id,
                        uint32_t depth,
                        bool success,
                        std::string_view error_kind,
                        uint32_t output_chars);
  void EmitSubAgentExtractFailed(std::string_view invocation_id,
                                   std::string_view json_path);

 protected:
  static int64_t NowMicros();
};

// No-op emitter — used as default when caller doesn't pass one in.
class NullEventEmitter : public EventEmitter {
 public:
  void Emit(proto::TraceEvent) override {}
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_EVENT_H_
