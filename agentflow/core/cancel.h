// agentflow/core/cancel.h
#ifndef AGENTFLOW_CORE_CANCEL_H_
#define AGENTFLOW_CORE_CANCEL_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>

namespace agentflow {

class CancelSource;

class CancelToken {
 public:
  CancelToken() = default;
  [[nodiscard]] bool IsCancelled() const noexcept;
  void OnCancel(std::function<void()> cb) const;
  asio::awaitable<void> WaitCancelled() const;

 private:
  friend class CancelSource;
  struct State {
    std::atomic<bool> cancelled{false};
    std::mutex mu;
    std::vector<std::function<void()>> on_cancel_cbs;
  };
  explicit CancelToken(std::shared_ptr<State> state) : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};

class CancelSource {
 public:
  CancelSource();
  [[nodiscard]] CancelToken Token() const { return CancelToken(state_); }
  void Cancel();
  [[nodiscard]] bool IsCancelled() const noexcept;

 private:
  std::shared_ptr<CancelToken::State> state_;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_CANCEL_H_
