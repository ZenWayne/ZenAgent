// agentflow/core/stub_node.h
#ifndef AGENTFLOW_CORE_STUB_NODE_H_
#define AGENTFLOW_CORE_STUB_NODE_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <utility>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "agentflow/core/node.h"

namespace agentflow {

// Node that:
//  - sleeps for `delay` (cancellable),
//  - calls `body(state)` to mutate state,
//  - records its execution order in `counter`.
class StubNode : public Node {
 public:
  using Body = std::function<void(State&)>;

  StubNode(std::string id, std::chrono::milliseconds delay,
           std::shared_ptr<std::atomic<int>> counter,
           Body body = {})
      : id_(std::move(id)),
        delay_(delay),
        counter_(std::move(counter)),
        body_(std::move(body)) {}

  std::string_view Id() const override { return id_; }
  std::string_view Kind() const override { return "stub"; }

  asio::awaitable<State> Run(State state, const CancelToken& cancel,
                              EventEmitter& emit) override {
    emit.EmitNodeStart(id_);
    if (delay_.count() > 0) {
      auto exec = co_await asio::this_coro::executor;
      auto end = std::chrono::steady_clock::now() + delay_;
      while (std::chrono::steady_clock::now() < end) {
        if (cancel.IsCancelled()) {
          emit.EmitNodeEnd(id_, /*cancelled=*/true, /*failed=*/false);
          co_return std::move(state);
        }
        asio::steady_timer step(exec, std::chrono::milliseconds(10));
        co_await step.async_wait(asio::use_awaitable);
      }
    }
    if (body_) body_(state);
    if (counter_) order_ = counter_->fetch_add(1);
    emit.EmitNodeEnd(id_, /*cancelled=*/false, /*failed=*/false);
    co_return std::move(state);
  }

  int OrderRan() const { return order_; }

 private:
  std::string id_;
  std::chrono::milliseconds delay_;
  std::shared_ptr<std::atomic<int>> counter_;
  Body body_;
  int order_ = -1;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_STUB_NODE_H_
