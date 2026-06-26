#ifndef AGENTFLOW_CORE_TOKEN_SINK_H_
#define AGENTFLOW_CORE_TOKEN_SINK_H_

#include <functional>
#include <string_view>

namespace agentflow {

// A direct, in-process stream of generated text deltas from a producer (an
// AgentNode, or a delegated sub-agent) to a consumer at the top of the stack
// (the JNI bridge, which marshals each token onto a Kotlin callback / Flow; or
// any C++ driver).
//
// This is the "direct to top" streaming path — leaner than wrapping every token
// in a proto::TraceEvent. The sink is called once per text delta, in order, on
// the io thread that runs the agent graph.
//
// Many-to-one by construction: the main agent and every delegated sub-agent
// share one sink, so fan-in needs no extra plumbing (a callback is inherently
// many-to-one). An empty sink means "don't stream". The sink runs synchronously
// on a single, cooperative io thread, so it needs no locking and must not block.
using TokenSink = std::function<void(std::string_view delta)>;

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_TOKEN_SINK_H_
