#ifndef AGENTFLOW_CORE_TOKEN_CHANNEL_H_
#define AGENTFLOW_CORE_TOKEN_CHANNEL_H_

#include <string>

#include <asio/error.hpp>
#include <asio/experimental/concurrent_channel.hpp>

namespace agentflow {

// A direct, in-process stream of generated text deltas from a producer (an
// AgentNode) to a consumer at the top of the stack (the JNI bridge, which
// marshals each token onto a Kotlin callback / Flow; or any C++ driver).
//
// This is the "direct to top" streaming path — leaner than wrapping every
// token in a proto::TraceEvent. The producer co_await async_send()s each delta;
// the consumer co_await async_receive()s them. End of stream is signalled by
// close() (async_receive then completes with an error code).
//
// Single io_context, cooperative: producer and consumer run on the same io
// thread, so no extra locking is needed.
using TokenChannel =
    asio::experimental::concurrent_channel<void(asio::error_code, std::string)>;

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_TOKEN_CHANNEL_H_
