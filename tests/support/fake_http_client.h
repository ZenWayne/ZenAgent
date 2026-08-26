// tests/support/fake_http_client.h
//
// Replays canned SSE frames and canned non-streaming bodies so the OpenAI
// backend's request building, mapping and retry policy can be tested with no
// network.
#ifndef TESTS_SUPPORT_FAKE_HTTP_CLIENT_H_
#define TESTS_SUPPORT_FAKE_HTTP_CLIENT_H_

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "agentflow/net/http_client.h"

namespace agentflow::testing {

// One scripted attempt: either a status to fail with, or frames to deliver
// (optionally emitting some frames BEFORE failing, to exercise the
// "no retry after a token was emitted" rule).
struct FakeHttpTurn {
  std::vector<std::string> frames;   // SSE data payloads
  absl::Status status = absl::OkStatus();
  std::string body;                  // for Post()
};

class FakeHttpClient : public net::IHttpClient {
 public:
  explicit FakeHttpClient(std::vector<FakeHttpTurn> turns)
      : turns_(turns.begin(), turns.end()) {}

  asio::awaitable<absl::Status> PostSse(net::HttpRequest req,
                                         const net::SseHandler& on_event,
                                         const CancelToken& cancel) override {
    requests_.push_back(req);
    if (turns_.empty()) co_return absl::UnavailableError("fake: no turns left");
    FakeHttpTurn turn = std::move(turns_.front());
    turns_.pop_front();
    for (const auto& f : turn.frames) {
      if (cancel.IsCancelled()) co_return absl::CancelledError("cancelled");
      if (on_event) co_await on_event(f);
    }
    co_return turn.status;
  }

  asio::awaitable<absl::StatusOr<std::string>> Post(
      net::HttpRequest req, const CancelToken&) override {
    requests_.push_back(req);
    if (turns_.empty()) co_return absl::UnavailableError("fake: no turns left");
    FakeHttpTurn turn = std::move(turns_.front());
    turns_.pop_front();
    if (!turn.status.ok()) co_return turn.status;
    co_return turn.body;
  }

  // Every request received, in order. Lets a test assert the request body and
  // that credentials went into a header rather than the body.
  const std::vector<net::HttpRequest>& requests() const { return requests_; }
  int attempts() const { return static_cast<int>(requests_.size()); }

 private:
  std::deque<FakeHttpTurn> turns_;
  std::vector<net::HttpRequest> requests_;
};

}  // namespace agentflow::testing
#endif  // TESTS_SUPPORT_FAKE_HTTP_CLIENT_H_
