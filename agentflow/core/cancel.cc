// agentflow/core/cancel.cc
#include "agentflow/core/cancel.h"

#include <chrono>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

namespace agentflow {

CancelSource::CancelSource() : state_(std::make_shared<CancelToken::State>()) {}

void CancelSource::Cancel() {
  std::vector<std::function<void()>> to_fire;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    bool already = state_->cancelled.exchange(true);
    if (already) return;
    to_fire = std::move(state_->on_cancel_cbs);
    state_->on_cancel_cbs.clear();
  }
  for (auto& cb : to_fire) cb();
}

bool CancelSource::IsCancelled() const noexcept {
  return state_->cancelled.load();
}

bool CancelToken::IsCancelled() const noexcept {
  return state_ && state_->cancelled.load();
}

void CancelToken::OnCancel(std::function<void()> cb) const {
  if (!state_) return;
  bool already = false;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->cancelled.load()) {
      already = true;
    } else {
      state_->on_cancel_cbs.push_back(std::move(cb));
    }
  }
  if (already) cb();
}

// Polling implementation: lightweight and avoids cross-executor wakeup
// complexity in P1. Refined to event-driven in a later iteration if the 50 ms
// latency becomes a problem.
asio::awaitable<void> CancelToken::WaitCancelled() const {
  if (!state_) co_return;
  auto executor = co_await asio::this_coro::executor;
  asio::steady_timer timer(executor);
  while (!state_->cancelled.load()) {
    timer.expires_after(std::chrono::milliseconds(50));
    co_await timer.async_wait(asio::use_awaitable);
  }
  co_return;
}

}  // namespace agentflow
