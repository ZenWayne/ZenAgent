// agentflow/observability/jsonl_event_emitter.cc
#include "agentflow/observability/jsonl_event_emitter.h"

#include <ostream>
#include <string>

#include <google/protobuf/util/json_util.h>

namespace agentflow {

JsonlEventEmitter::JsonlEventEmitter(std::ostream& out) : out_(out) {}

void JsonlEventEmitter::Emit(proto::TraceEvent ev) {
  std::string line;
  google::protobuf::util::JsonPrintOptions opts;
  opts.add_whitespace = false;
  opts.always_print_fields_with_no_presence = true;
  (void)google::protobuf::util::MessageToJsonString(ev, &line, opts);
  std::lock_guard<std::mutex> lk(mu_);
  out_ << line << '\n';
}

}  // namespace agentflow
