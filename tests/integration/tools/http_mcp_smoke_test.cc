// tests/integration/tools/http_mcp_smoke_test.cc
//
// Drives the real McpClient over MCP streamable-HTTP against a tiny Python
// server living next to this file. Validates the handshake, the assigned
// Mcp-Session-Id round trip, tools discovery and one real tools/call over an
// actual socket — the FakeHttpClient unit tests cannot cover chunking or
// connection close.
//
// Also directly exercises net::HttpsClient (no McpClient in the way) against
// a non-2xx route to prove that Post() fills the HttpResponseHead out-param
// even when the response is an error: MCP relies on this to detect an
// expired session from a 404's headers (see design spec + Task 2).

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>

#include "absl/status/status.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/net/https_client.h"
#include "agentflow/tools/tool_registry.h"
#include "mcp_spec.pb.h"

namespace agentflow {
namespace {

using json = nlohmann::json;

// Same runfiles layout as stdio_mcp_smoke_test: "<TEST_SRCDIR>/_main/<pkg>".
std::string EchoHttpServerPath() {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  if (srcdir != nullptr) {
    std::string candidate =
        std::string(srcdir) +
        "/_main/tests/integration/tools/echo_mcp_http_server.py";
    struct stat st;
    if (::stat(candidate.c_str(), &st) == 0) return candidate;
  }
  return "tests/integration/tools/echo_mcp_http_server.py";
}

// Starts the Python fake server via fork()+exec() and returns its bound port.
// Lifecycle is fork/exec + SIGTERM, not popen()+stdin-EOF: under bazel test a
// child's stdin is normally /dev/null, so `for line in sys.stdin` would give
// the server an immediate spurious EOF before the test even connects; and if
// stdin stayed open instead, pclose() would block forever waiting for a
// process that has no reason to exit. Signaling the child directly avoids
// both failure modes and is symmetric with how a test harness would manage
// a real long-lived server process.
class EchoServer {
 public:
  // Starts the child. Cleans up any partially-started child on failure so a
  // failed Start() never leaks an orphan holding the port.
  bool Start() {
    int out_pipe[2];
    if (::pipe(out_pipe) != 0) return false;

    const pid_t pid = ::fork();
    if (pid < 0) {
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
      return false;
    }

    if (pid == 0) {
      // Child: stdout -> pipe write end, then exec the Python server.
      ::dup2(out_pipe[1], STDOUT_FILENO);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
      const std::string path = EchoHttpServerPath();
      ::execl("/usr/bin/python3", "/usr/bin/python3", path.c_str(),
              static_cast<char*>(nullptr));
      // execl only returns on failure.
      std::fprintf(stderr, "execl failed: %s\n", std::strerror(errno));
      _exit(127);
    }

    // Parent.
    ::close(out_pipe[1]);
    pid_ = pid;
    read_fd_ = out_pipe[0];

    // Read the "PORT <n>\n" line the server prints on startup.
    std::string line;
    char c = 0;
    while (line.find('\n') == std::string::npos) {
      const ssize_t n = ::read(read_fd_, &c, 1);
      if (n <= 0) {
        Reap();
        return false;
      }
      line.push_back(c);
      if (line.size() > 256) {
        Reap();
        return false;
      }
    }
    int p = 0;
    if (std::sscanf(line.c_str(), "PORT %d", &p) != 1 || p <= 0) {
      Reap();
      return false;
    }
    port_ = p;
    return true;
  }

  ~EchoServer() { Reap(); }

  int port() const { return port_; }

 private:
  // Kills and waits for the child so a failed test (or an ASSERT_* early
  // return) never leaves an orphan process pinning the port. Idempotent.
  void Reap() {
    if (read_fd_ >= 0) {
      ::close(read_fd_);
      read_fd_ = -1;
    }
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      int status = 0;
      ::waitpid(pid_, &status, 0);
      pid_ = -1;
    }
  }

  pid_t pid_ = -1;
  int read_fd_ = -1;
  int port_ = 0;
};

TEST(HttpMcpSmokeTest, AttachAndInvokeEcho) {
  EchoServer server;
  ASSERT_TRUE(server.Start())
      << "failed to start " << EchoHttpServerPath();

  proto::McpServerSpec spec;
  spec.set_transport(proto::McpServerSpec::HTTP_SSE);
  spec.set_command_or_url("http://127.0.0.1:" + std::to_string(server.port()) +
                          "/mcp");

  asio::io_context io;
  ToolRegistry reg(io);

  struct Outcome {
    absl::Status attach_status;
    bool invoke_threw = false;
    std::string invoke_error;
    std::string invoke_result;
    std::string export_listing;
  } outcome;

  std::string io_error;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        try {
          outcome.attach_status = co_await reg.AttachMcpServer(spec);
          std::vector<std::string> ask = {"echo"};
          outcome.export_listing = reg.ExportToolsJson(ask);
        } catch (const std::exception& e) {
          outcome.attach_status = absl::InternalError(
              std::string("attach/list threw: ") + e.what());
          co_return;
        }
        try {
          outcome.invoke_result =
              co_await reg.Invoke("echo", R"({"text":"hi over http"})",
                                  CancelToken{});
        } catch (const std::exception& e) {
          outcome.invoke_threw = true;
          outcome.invoke_error = e.what();
        }
        reg.ShutdownMcp();
      },
      asio::detached);

  try {
    io.run();
  } catch (const std::exception& e) {
    io_error = e.what();
  }
  ASSERT_TRUE(io_error.empty()) << "io.run() threw: " << io_error;

  ASSERT_TRUE(outcome.attach_status.ok())
      << "AttachMcpServer failed: " << outcome.attach_status.message();
  EXPECT_NE(outcome.export_listing.find("\"echo\""), std::string::npos)
      << "tool list missing echo; got: " << outcome.export_listing;
  ASSERT_FALSE(outcome.invoke_threw) << "Invoke threw: " << outcome.invoke_error;

  auto parsed = json::parse(outcome.invoke_result);
  ASSERT_TRUE(parsed["content"].is_array());
  ASSERT_FALSE(parsed["content"].empty());
  EXPECT_EQ(parsed["content"][0]["text"], "hi over http");
}

// Closes the gap Task 2 left unverified at the execution level: IHttpClient
// ::Post must fill `out_head` (status code + headers) BEFORE returning a
// non-OK Status for a non-2xx response, not only on the 2xx path. MCP relies
// on exactly this to read a 404's headers when a session has expired. The
// FakeHttpClient-based unit tests can't cover this because the fake fills in
// whatever the test tells it to; only the real HttpsClient's actual response
// parsing order is worth anything here. This talks to HttpsClient directly —
// no McpClient, no ToolRegistry.
TEST(HttpMcpSmokeTest, DirectHttpsClientNon2xxStillFillsOutHead) {
  EchoServer server;
  ASSERT_TRUE(server.Start())
      << "failed to start " << EchoHttpServerPath();

  asio::io_context io;
  net::HttpsClientOptions opts;  // plain http:// -> ca_path unused
  net::HttpsClient http(io, opts);

  net::HttpRequest req;
  req.url =
      "http://127.0.0.1:" + std::to_string(server.port()) + "/notfound";
  req.body = "{}";
  req.headers = {{"content-type", "application/json"}};

  struct Outcome {
    bool ok = false;
    std::string error_message;
    net::HttpResponseHead head;
  } outcome;

  std::string io_error;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto result =
            co_await http.Post(req, CancelToken{}, &outcome.head);
        outcome.ok = result.ok();
        if (!result.ok()) outcome.error_message = result.status().ToString();
      },
      asio::detached);

  try {
    io.run();
  } catch (const std::exception& e) {
    io_error = e.what();
  }
  ASSERT_TRUE(io_error.empty()) << "io.run() threw: " << io_error;

  EXPECT_FALSE(outcome.ok) << "expected a non-OK status for a 404 response";
  EXPECT_EQ(outcome.head.status_code, 404);

  bool found_probe_header = false;
  for (const auto& [name, value] : outcome.head.headers) {
    if (name == "x-probe" && value == "yes") {
      found_probe_header = true;
      break;
    }
  }
  EXPECT_TRUE(found_probe_header)
      << "expected X-Probe: yes header on the 404 response; got "
      << outcome.head.headers.size() << " headers, status "
      << outcome.head.status_code << ", error: " << outcome.error_message;
}

}  // namespace
}  // namespace agentflow
