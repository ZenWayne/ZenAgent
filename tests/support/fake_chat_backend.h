// tests/support/fake_chat_backend.h
//
// A scripted IChatBackend for tests: no engine, no network. Each SendAsync
// call pops the next canned canonical response. Used by chat_backend_test,
// agent_node_test and llm_node_test.
#ifndef TESTS_SUPPORT_FAKE_CHAT_BACKEND_H_
#define TESTS_SUPPORT_FAKE_CHAT_BACKEND_H_

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <asio/awaitable.hpp>

#include "agentflow/inference/chat_backend.h"

namespace agentflow::testing {

class FakeConversation : public IConversation {
 public:
  FakeConversation(std::deque<std::string> responses,
                    std::vector<std::string> deltas)
      : responses_(std::move(responses)), deltas_(std::move(deltas)) {}

  asio::awaitable<absl::StatusOr<std::string>> SendAsync(
      std::string message_json, const TokenSink& on_token,
      const CancelToken& cancel) override {
    sent_.push_back(std::move(message_json));
    if (cancel.IsCancelled()) {
      co_return absl::CancelledError("cancelled");
    }
    if (on_token) {
      for (const auto& d : deltas_) co_await on_token(d);
    }
    if (responses_.empty()) {
      co_return absl::UnavailableError("fake: no scripted response left");
    }
    std::string r = std::move(responses_.front());
    responses_.pop_front();
    co_return r;
  }

  void Cancel() override { ++cancel_calls_; }

  // Messages this conversation received, in order. Lets a test assert what
  // AgentNode sent back after dispatching a tool.
  const std::vector<std::string>& sent() const { return sent_; }
  int cancel_calls() const { return cancel_calls_; }

 private:
  std::deque<std::string> responses_;
  std::vector<std::string> deltas_;
  std::vector<std::string> sent_;
  int cancel_calls_ = 0;
};

class FakeChatBackend : public IChatBackend {
 public:
  explicit FakeChatBackend(std::vector<std::string> responses)
      : responses_(responses.begin(), responses.end()) {}

  // Text deltas handed to the TokenSink on every SendAsync.
  void set_deltas(std::vector<std::string> deltas) {
    deltas_ = std::move(deltas);
  }

  std::shared_ptr<IConversation> CreateConversation(
      ChatConversationOptions opts) override {
    last_options_ = std::move(opts);
    auto conv = std::make_shared<FakeConversation>(responses_, deltas_);
    last_conversation_ = conv;
    return conv;
  }

  std::string_view Describe() const override { return "fake"; }

  // Options the node passed in — lets a test assert the system prompt and
  // tools JSON were built correctly.
  const ChatConversationOptions& last_options() const { return last_options_; }

  // Held STRONGLY (not weak_ptr): the fake exists precisely so tests can
  // observe the conversation after a run completes, and nothing else in a
  // test necessarily keeps it alive.
  std::shared_ptr<FakeConversation> last_conversation() const {
    return last_conversation_;
  }

 private:
  std::deque<std::string> responses_;
  std::vector<std::string> deltas_;
  ChatConversationOptions last_options_;
  std::shared_ptr<FakeConversation> last_conversation_;
};

}  // namespace agentflow::testing
#endif  // TESTS_SUPPORT_FAKE_CHAT_BACKEND_H_
