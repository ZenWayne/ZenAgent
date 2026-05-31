// agentflow/tools/mcp_client.cc
#include "agentflow/tools/mcp_client.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/post.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <nlohmann/json.hpp>

namespace agentflow::mcp {

namespace {

using json = nlohmann::json;

constexpr const char* kJsonRpc = "2.0";
constexpr const char* kProtocolVersion = "2025-03-26";
constexpr const char* kClientName = "agentflow";
constexpr const char* kClientVersion = "0.1.0";

ToolSchema ToolSchemaFromMcp(const json& tool) {
  ToolSchema s;
  s.name = tool.value("name", "");
  s.description = tool.value("description", "");
  // MCP returns `inputSchema` as a JSON object; serialize back to a string
  // for ToolSchema::params_json_schema. nlohmann::json default-constructs to
  // null, which serializes to "null"; coerce that to "{}".
  if (tool.contains("inputSchema") && tool["inputSchema"].is_object()) {
    s.params_json_schema = tool["inputSchema"].dump();
  } else {
    s.params_json_schema = "{}";
  }
  return s;
}

}  // namespace

class McpClient::Impl
    : public std::enable_shared_from_this<McpClient::Impl> {
 public:
  Impl(proto::McpServerSpec spec, asio::io_context& io)
      : spec_(std::move(spec)), io_(io) {}

  ~Impl() { ShutdownInternal(); }

  asio::awaitable<absl::Status> Connect() {
    if (shutdown_) co_return absl::CancelledError("client shut down");
    if (state_ == State::kReady) co_return absl::OkStatus();

    // Reconnect: drop a previously broken transport before retrying.
    if (state_ == State::kBroken) {
      ShutdownInternal(/*permanent=*/false);
    }

    // Single-flight: while a Connect is in progress, queue ourselves on a
    // waiter channel; the first connector wakes everyone with the result.
    if (state_ == State::kConnecting) {
      auto ch = MakeWaiter();
      connect_waiters_.push_back(ch);
      co_await ch->async_receive(
          asio::as_tuple(asio::use_awaitable));
      co_return state_ == State::kReady
          ? absl::OkStatus()
          : absl::UnavailableError("MCP server failed to start");
    }

    state_ = State::kConnecting;
    absl::Status status;
    switch (spec_.transport()) {
      case proto::McpServerSpec::STDIO:
        status = co_await ConnectStdio();
        break;
      default:
        status = absl::UnimplementedError(
            "MCP transport not implemented (only STDIO in P3)");
        break;
    }
    state_ = status.ok() ? State::kReady : State::kBroken;
    NotifyConnectWaiters();
    co_return status;
  }

  asio::awaitable<absl::StatusOr<std::vector<ToolSchema>>> ListTools() {
    if (auto s = co_await EnsureReady(); !s.ok()) co_return s;

    static const CancelToken kNoCancel;
    auto result = co_await SendRequest("tools/list", json::object(), kNoCancel);
    if (!result.ok()) co_return result.status();

    std::vector<ToolSchema> out;
    if (result->contains("tools") && (*result)["tools"].is_array()) {
      for (const auto& t : (*result)["tools"]) {
        out.push_back(ToolSchemaFromMcp(t));
      }
    }
    co_return out;
  }

  asio::awaitable<absl::StatusOr<std::string>> CallTool(
      std::string_view name, std::string_view args_json,
      const CancelToken& cancel) {
    if (auto s = co_await EnsureReady(); !s.ok()) co_return s;

    json arguments = json::object();
    if (!args_json.empty()) {
      try {
        arguments = json::parse(args_json);
      } catch (const json::parse_error& e) {
        co_return absl::InvalidArgumentError(
            std::string("args_json parse error: ") + e.what());
      }
    }
    json params = {{"name", std::string(name)}, {"arguments", arguments}};

    auto result = co_await SendRequest("tools/call", std::move(params), cancel);
    if (!result.ok()) co_return result.status();
    co_return result->dump();
  }

  void Shutdown() { ShutdownInternal(/*permanent=*/true); }

 private:
  // ── connection state ──────────────────────────────────────────────────────

  enum class State { kIdle, kConnecting, kReady, kBroken };

  using WaiterChannel =
      asio::experimental::channel<void(asio::error_code, int)>;
  using RespChannel =
      asio::experimental::channel<void(asio::error_code, json)>;

  std::shared_ptr<WaiterChannel> MakeWaiter() {
    return std::make_shared<WaiterChannel>(io_, 1);
  }

  void NotifyConnectWaiters() {
    for (auto& ch : connect_waiters_) {
      asio::error_code ec;
      ch->try_send(ec, 0);
    }
    connect_waiters_.clear();
  }

  asio::awaitable<absl::Status> EnsureReady() {
    if (state_ == State::kReady) co_return absl::OkStatus();
    co_return co_await Connect();
  }

  // ── stdio transport + handshake ───────────────────────────────────────────

  asio::awaitable<absl::Status> ConnectStdio() {
    if (spec_.command_or_url().empty()) {
      co_return absl::InvalidArgumentError(
          "STDIO McpServerSpec.command_or_url is empty");
    }
    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    if (::pipe(to_child) < 0 || ::pipe(from_child) < 0) {
      const int err = errno;
      if (to_child[0] >= 0) { ::close(to_child[0]); ::close(to_child[1]); }
      co_return absl::InternalError(std::string("pipe() failed: ") +
                                    std::strerror(err));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
      const int err = errno;
      ::close(to_child[0]); ::close(to_child[1]);
      ::close(from_child[0]); ::close(from_child[1]);
      co_return absl::InternalError(std::string("fork() failed: ") +
                                    std::strerror(err));
    }
    if (pid == 0) {
      // Child. Wire stdin/stdout to the pipes, leave stderr alone.
      ::dup2(to_child[0], STDIN_FILENO);
      ::dup2(from_child[1], STDOUT_FILENO);
      ::close(to_child[0]); ::close(to_child[1]);
      ::close(from_child[0]); ::close(from_child[1]);

      std::vector<std::string> argv_storage;
      argv_storage.push_back(spec_.command_or_url());
      for (const auto& a : spec_.args()) argv_storage.push_back(a);
      std::vector<char*> argv;
      argv.reserve(argv_storage.size() + 1);
      for (auto& s : argv_storage) argv.push_back(s.data());
      argv.push_back(nullptr);

      ::execvp(spec_.command_or_url().c_str(), argv.data());
      // execvp only returns on failure.
      std::fprintf(stderr, "agentflow MCP stdio exec failed: %s\n",
                   std::strerror(errno));
      ::_exit(127);
    }

    // Parent. Close child's ends of the pipes.
    ::close(to_child[0]);
    ::close(from_child[1]);
    child_pid_ = pid;
    child_in_ = std::make_unique<asio::posix::stream_descriptor>(
        io_, to_child[1]);
    child_out_ = std::make_unique<asio::posix::stream_descriptor>(
        io_, from_child[0]);

    // Reap the child on exit if we never get a clean Shutdown().
    asio::co_spawn(io_,
        [self = shared_from_this()]() -> asio::awaitable<void> {
          co_await self->ReadLoop();
        },
        asio::detached);

    // MCP handshake: `initialize` then `notifications/initialized`.
    // We're temporarily kReady from the transport's POV so SendRequest works;
    // any failure flips us back to kBroken in the caller.
    state_ = State::kReady;
    json init_params = {
        {"protocolVersion", kProtocolVersion},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", kClientName}, {"version", kClientVersion}}},
    };
    static const CancelToken kNoCancel;
    auto init = co_await SendRequest("initialize", init_params, kNoCancel);
    if (!init.ok()) co_return init.status();

    SendNotification("notifications/initialized", json::object());
    co_return absl::OkStatus();
  }

  // ── JSON-RPC framing ──────────────────────────────────────────────────────

  asio::awaitable<absl::StatusOr<json>> SendRequest(
      std::string method, json params, const CancelToken& cancel) {
    if (state_ != State::kReady) {
      co_return absl::FailedPreconditionError("MCP client not connected");
    }
    const int64_t id = next_id_++;
    auto ch = std::make_shared<RespChannel>(io_, 1);
    pending_[id] = ch;

    // Wire cancellation to closing the per-request channel + best-effort
    // notify the server. The OnCancel callback may run on any thread (per
    // CancelSource semantics), so post onto our io_context.
    cancel.OnCancel([self = shared_from_this(), id]() {
      asio::post(self->io_, [self, id]() {
        auto it = self->pending_.find(id);
        if (it == self->pending_.end()) return;
        auto ch = it->second;
        self->pending_.erase(it);
        ch->close();
        self->SendNotification(
            "notifications/cancelled",
            {{"requestId", id}, {"reason", "client cancelled"}});
      });
    });

    json msg = {
        {"jsonrpc", kJsonRpc},
        {"id", id},
        {"method", method},
        {"params", std::move(params)},
    };
    WriteLine(msg.dump());

    auto [ec, payload] = co_await ch->async_receive(
        asio::as_tuple(asio::use_awaitable));
    if (ec) {
      // operation_aborted = channel closed by cancel; broken_pipe / eof =
      // transport gone.
      if (ec == asio::error::operation_aborted) {
        co_return absl::CancelledError("MCP call cancelled");
      }
      co_return absl::UnavailableError(
          std::string("MCP transport error: ") + ec.message());
    }
    if (payload.contains("error")) {
      const auto& err = payload["error"];
      co_return absl::InternalError(
          "MCP error " +
          std::to_string(err.value("code", -1)) + ": " +
          err.value("message", std::string{}));
    }
    co_return payload.value("result", json::object());
  }

  void SendNotification(std::string method, json params) {
    if (!child_in_) return;
    json msg = {
        {"jsonrpc", kJsonRpc},
        {"method", std::move(method)},
        {"params", std::move(params)},
    };
    WriteLine(msg.dump());
  }

  // Append a newline-delimited line and kick the writer if idle.
  void WriteLine(std::string line) {
    line.push_back('\n');
    write_queue_.push_back(std::move(line));
    if (!write_active_) PumpWriter();
  }

  void PumpWriter() {
    if (write_queue_.empty() || !child_in_) {
      write_active_ = false;
      return;
    }
    write_active_ = true;
    auto self = shared_from_this();
    auto& front = write_queue_.front();
    asio::async_write(
        *child_in_, asio::buffer(front),
        [self](const asio::error_code& ec, std::size_t) {
          self->write_queue_.pop_front();
          if (ec) {
            // Transport died; fail all in-flight requests.
            self->write_active_ = false;
            self->FailPending(ec);
            self->state_ = McpClient::Impl::State::kBroken;
            return;
          }
          self->PumpWriter();
        });
  }

  // ── reader loop ───────────────────────────────────────────────────────────

  asio::awaitable<void> ReadLoop() {
    while (child_out_) {
      auto [ec, n] = co_await asio::async_read_until(
          *child_out_, read_buf_, '\n',
          asio::as_tuple(asio::use_awaitable));
      (void)n;
      if (ec) {
        FailPending(ec);
        state_ = State::kBroken;
        co_return;
      }
      std::istream is(&read_buf_);
      std::string line;
      std::getline(is, line);
      if (line.empty()) continue;
      try {
        DispatchInbound(json::parse(line));
      } catch (const json::parse_error&) {
        // Non-fatal: drop malformed line, server may resync.
      }
    }
  }

  void DispatchInbound(const json& msg) {
    if (!msg.contains("id") || msg["id"].is_null()) {
      // Notification — we currently ignore server-initiated notifications.
      return;
    }
    int64_t id = 0;
    try {
      id = msg["id"].get<int64_t>();
    } catch (const json::type_error&) {
      return;  // protocol violation; drop
    }
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    auto ch = it->second;
    pending_.erase(it);
    asio::error_code ec;
    ch->try_send(ec, msg);
  }

  void FailPending(const asio::error_code& /*ec*/) {
    for (auto& [_, ch] : pending_) ch->close();
    pending_.clear();
  }

  // ── shutdown ──────────────────────────────────────────────────────────────

  void ShutdownInternal(bool permanent = true) {
    if (permanent) shutdown_ = true;
    if (child_in_) { child_in_->close(); child_in_.reset(); }
    if (child_out_) { child_out_->close(); child_out_.reset(); }
    FailPending(asio::error::operation_aborted);
    NotifyConnectWaiters();
    if (child_pid_ > 0) {
      ::kill(child_pid_, SIGTERM);
      // Best-effort reap without blocking the io_context.
      int status = 0;
      const pid_t r = ::waitpid(child_pid_, &status, WNOHANG);
      if (r == 0) {
        // Still alive; escalate.
        ::kill(child_pid_, SIGKILL);
        ::waitpid(child_pid_, &status, 0);
      }
      child_pid_ = -1;
    }
    if (state_ == State::kReady) state_ = State::kBroken;
  }

  proto::McpServerSpec spec_;
  asio::io_context& io_;

  State state_ = State::kIdle;
  std::vector<std::shared_ptr<WaiterChannel>> connect_waiters_;

  pid_t child_pid_ = -1;
  std::unique_ptr<asio::posix::stream_descriptor> child_in_;
  std::unique_ptr<asio::posix::stream_descriptor> child_out_;
  asio::streambuf read_buf_;

  int64_t next_id_ = 1;
  std::unordered_map<int64_t, std::shared_ptr<RespChannel>> pending_;

  std::deque<std::string> write_queue_;
  bool write_active_ = false;
  bool shutdown_ = false;
};

// ── McpClient facade ─────────────────────────────────────────────────────────

std::shared_ptr<McpClient> McpClient::Create(proto::McpServerSpec spec,
                                             asio::io_context& io) {
  // Impl must be shared so enable_shared_from_this works (Connect() spawns
  // a detached ReadLoop coroutine and SendRequest registers an OnCancel
  // callback, both of which capture shared_from_this()).
  auto impl = std::make_shared<Impl>(std::move(spec), io);
  return std::shared_ptr<McpClient>(new McpClient(std::move(impl)));
}

McpClient::McpClient(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
McpClient::~McpClient() = default;

asio::awaitable<absl::Status> McpClient::Connect() {
  return impl_->Connect();
}
asio::awaitable<absl::StatusOr<std::vector<ToolSchema>>> McpClient::ListTools() {
  return impl_->ListTools();
}
asio::awaitable<absl::StatusOr<std::string>> McpClient::CallTool(
    std::string_view name, std::string_view args_json,
    const CancelToken& cancel) {
  return impl_->CallTool(name, args_json, cancel);
}
void McpClient::Shutdown() { impl_->Shutdown(); }

}  // namespace agentflow::mcp
