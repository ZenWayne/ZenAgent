// agentflow/tools/mcp_client.cc
#include "agentflow/tools/mcp_client.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
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

#include "agentflow/net/http_client.h"
#include "agentflow/net/https_client.h"
#include "agentflow/tools/mcp_http_codec.h"

namespace agentflow::mcp {

namespace {

using json = nlohmann::json;

constexpr const char* kJsonRpc = "2.0";
constexpr const char* kProtocolVersion = "2025-03-26";
constexpr const char* kClientName = "agentflow";
constexpr const char* kClientVersion = "0.1.0";
// Desktop default CA bundle, used when neither an injected http client nor
// MCP_CA_PATH supplies one. Same fallback as examples/remote-llm/main.cc.
constexpr const char* kDefaultCaPath = "/etc/ssl/certs/ca-certificates.crt";

std::string ToLowerAscii(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

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
  Impl(proto::McpServerSpec spec, asio::io_context& io,
       std::shared_ptr<net::IHttpClient> http = nullptr)
      : spec_(std::move(spec)), io_(io), http_(std::move(http)) {}

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
      case proto::McpServerSpec::HTTP_SSE:
        status = co_await ConnectHttp();
        break;
      default:
        status = absl::UnimplementedError(
            "MCP transport not implemented (only STDIO and HTTP_SSE)");
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

  // ── HTTP transport + handshake ────────────────────────────────────────────

  bool IsHttp() const {
    return spec_.transport() == proto::McpServerSpec::HTTP_SSE;
  }

  asio::awaitable<absl::Status> ConnectHttp() {
    if (spec_.command_or_url().empty()) {
      co_return absl::InvalidArgumentError(
          "HTTP McpServerSpec.command_or_url is empty");
    }
    if (http_ == nullptr) {
      net::HttpsClientOptions opts;
      // https:// requires a CA bundle (verification is mandatory, see
      // https_client.cc); plain http:// ignores it. MCP_CA_PATH lets ops
      // override; otherwise fall back to the desktop system bundle, same as
      // examples/remote-llm/main.cc.
      if (const char* ca = std::getenv("MCP_CA_PATH")) {
        opts.ca_path = ca;
      } else {
        opts.ca_path = kDefaultCaPath;
      }
      http_ = std::make_shared<net::HttpsClient>(io_, std::move(opts));
    }
    http_session_id_.clear();

    static const CancelToken kNoCancel;
    json init_params = {
        {"protocolVersion", kProtocolVersion},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", kClientName}, {"version", kClientVersion}}},
    };
    auto init = co_await PostRpc("initialize", std::move(init_params),
                                 /*is_notification=*/false, kNoCancel);
    if (!init.ok()) co_return init.status();

    // The server assigns the session on initialize; every later POST must
    // echo it back or the server answers 404.
    if (http_session_id_.empty()) {
      co_return absl::InternalError(
          "MCP HTTP: server did not assign an Mcp-Session-Id on initialize");
    }

    auto ack = co_await PostRpc("notifications/initialized", json::object(),
                                /*is_notification=*/true, kNoCancel);
    if (!ack.ok()) co_return ack.status();

    state_ = State::kReady;
    NotifyConnectWaiters();
    co_return absl::OkStatus();
  }

  // One JSON-RPC round trip over HTTP. Notifications carry no id and expect an
  // empty 202 body. Captures a newly assigned session id as a side effect.
  asio::awaitable<absl::StatusOr<json>> PostRpc(std::string method, json params,
                                                bool is_notification,
                                                const CancelToken& cancel) {
    json msg = {{"jsonrpc", kJsonRpc}, {"method", std::move(method)}};
    if (!params.is_null()) msg["params"] = std::move(params);
    int64_t request_id = 0;
    if (!is_notification) {
      request_id = next_id_++;
      msg["id"] = request_id;
    }

    net::HttpRequest req;
    req.url = spec_.command_or_url();
    req.body = msg.dump();
    req.headers = BuildMcpHttpHeaders(http_session_id_);
    for (const auto& [k, v] : spec_.headers()) {
      // Header names are compared case-insensitively everywhere else in this
      // feature (mcp_http_codec.cc); doing a case-sensitive match here would
      // let e.g. a spec-provided "content-type" append a duplicate header
      // instead of overriding BuildMcpHttpHeaders's "Content-Type".
      const std::string want = ToLowerAscii(k);
      auto it = std::find_if(
          req.headers.begin(), req.headers.end(),
          [&want](const auto& kv) { return ToLowerAscii(kv.first) == want; });
      if (it != req.headers.end()) {
        it->second = v;
      } else {
        req.headers.emplace_back(k, v);
      }
    }

    net::HttpResponseHead head;
    auto body = co_await http_->Post(std::move(req), cancel, &head);
    // Publish a newly assigned session id even on failure: https_client.cc
    // populates the head before its non-2xx bail-out precisely so callers can
    // read headers on a failure (e.g. a 404 from an expired session may still
    // echo the session it no longer recognizes).
    if (auto sid = SessionIdFromHeaders(head.headers); !sid.empty()) {
      http_session_id_ = sid;
    }
    if (!body.ok()) {
      // Mark the transport broken so the next EnsureReady() re-handshakes
      // instead of reusing a session the server has already discarded.
      // Cancellation is not a transport failure -- leave state_ alone so an
      // in-flight cancel doesn't spuriously break an otherwise-healthy
      // client.
      //
      // Deliberately do NOT clear http_session_id_ here: this client is
      // shared across concurrent CallTool/ListTools coroutines (one McpClient
      // per AttachMcpServer -- see tool_registry.cc), and another coroutine
      // may already be past the `state_ != kReady` gate in SendRequest,
      // between reading http_session_id_ into its request and actually
      // sending it. Clearing it here would make that concurrent request go
      // out with an EMPTY Mcp-Session-Id (a guaranteed spurious failure)
      // instead of a stale one (a pre-existing, self-healing race: the
      // server rejects it, that coroutine also marks the client kBroken, and
      // ConnectHttp() clears the session id itself at the start of every
      // handshake).
      if (!absl::IsCancelled(body.status())) {
        state_ = State::kBroken;
      }
      co_return body.status();
    }

    auto decoded = DecodeMcpHttpResponse(head.status_code, head.headers, *body);
    if (!decoded.ok()) co_return decoded.status();
    if (!decoded->has_value()) {
      // Empty 2xx body. Legitimate only for a notification ack; for a real
      // request this is a swallowed protocol violation -- the caller must not
      // see it as an empty-but-successful tool result.
      if (is_notification) co_return json::object();
      co_return absl::InternalError(
          "MCP HTTP: empty body for request id " +
          std::to_string(request_id));
    }

    const json& payload = **decoded;
    if (!payload.is_object()) {
      // `payload` is only guaranteed to be valid JSON, not an object --
      // `.value("id", ...)` etc. below would throw nlohmann::json::type_error
      // on e.g. `data: null` / `data: []` / `data: "ok"`. Bail before any of
      // that runs.
      co_return absl::InvalidArgumentError(
          "MCP HTTP: JSON-RPC payload is not an object: " + payload.dump());
    }
    // DecodeMcpHttpResponse picks the LAST SSE frame as a heuristic, not a
    // correlated match — a server that trails the real response with a
    // progress/notification frame must not have that frame silently accepted
    // as this call's result.
    if (!is_notification &&
        (!payload.contains("id") || payload["id"] != request_id)) {
      co_return absl::InternalError(
          "MCP HTTP: response id " + payload.value("id", json()).dump() +
          " does not match request id " + std::to_string(request_id));
    }
    if (payload.contains("error")) {
      const auto& err = payload["error"];
      if (!err.is_object()) {
        co_return absl::InternalError(
            "MCP HTTP: error field is not an object: " + err.dump());
      }
      co_return absl::InternalError(
          "MCP error " + std::to_string(err.value("code", -1)) + ": " +
          err.value("message", std::string{}));
    }
    if (!payload.contains("result")) {
      co_return absl::InternalError(
          "MCP HTTP: response has neither result nor error: " +
          payload.dump());
    }
    co_return payload["result"];
  }

  // ── JSON-RPC framing ──────────────────────────────────────────────────────

  asio::awaitable<absl::StatusOr<json>> SendRequest(
      std::string method, json params, const CancelToken& cancel) {
    if (state_ != State::kReady) {
      co_return absl::FailedPreconditionError("MCP client not connected");
    }
    if (IsHttp()) {
      co_return co_await PostRpc(std::move(method), std::move(params),
                                 /*is_notification=*/false, cancel);
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
    if (IsHttp()) {
      // No long-lived connection to tear down for HTTP — just drop the
      // session so the next Connect() re-handshakes. permanent=false is the
      // reconnect-from-kBroken path; permanent=true is a real Shutdown().
      http_session_id_.clear();
      NotifyConnectWaiters();
      state_ = permanent ? State::kBroken : State::kIdle;
      return;
    }
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

  std::shared_ptr<net::IHttpClient> http_;  // HTTP transports only
  std::string http_session_id_;             // assigned by initialize

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

std::shared_ptr<McpClient> McpClient::Create(
    proto::McpServerSpec spec, asio::io_context& io,
    std::shared_ptr<net::IHttpClient> http) {
  auto impl = std::make_shared<Impl>(std::move(spec), io, std::move(http));
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
