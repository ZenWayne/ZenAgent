#!/usr/bin/env python3
"""Minimal MCP streamable-HTTP server for the smoke test. Stdlib only.

Mirrors what FastMCP actually does (verified against video-maker-mcp):
  initialize                 -> 200 text/event-stream + Mcp-Session-Id header
  notifications/initialized  -> 202 application/json, empty body
  tools/list, tools/call     -> 200 text/event-stream

Also serves /notfound -> 404 with a custom X-Probe header, so callers can
verify that HttpsClient::Post fills the HttpResponseHead out-param even on a
non-2xx response (see the DirectHttpsClientNon2xx test in
http_mcp_smoke_test.cc).

Prints the bound port on stdout as "PORT <n>" so the test can find it.

Lifecycle: runs serve_forever() until killed by SIGTERM (the parent test
process does `kill(pid, SIGTERM)` then waitpid()). Does NOT read stdin to
decide when to exit -- under bazel test a child's stdin is typically
/dev/null, so blocking on stdin EOF would either exit immediately (spuriously,
before the test even connects) or, if stdin were kept open, hang forever
waiting for something that's never written. Signal-based shutdown avoids both
failure modes.
"""
import json
import signal
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

SESSION = "testsession0123456789abcdef0123"

TOOLS = [{
    "name": "echo",
    "description": "echoes its argument back",
    "inputSchema": {"type": "object",
                    "properties": {"text": {"type": "string"}}},
}]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _sse(self, payload):
        frame = f"event: message\ndata: {json.dumps(payload)}\n\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(frame)))
        if self.headers.get("Mcp-Session-Id") is None:
            self.send_header("Mcp-Session-Id", SESSION)
        self.end_headers()
        self.wfile.write(frame)

    def _not_found_probe(self):
        body = b"not found"
        self.send_response(404)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Probe", "yes")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/notfound":
            self._not_found_probe()
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        if self.path == "/notfound":
            # Drain the request body so keep-alive framing stays correct.
            n = int(self.headers.get("Content-Length", "0"))
            if n:
                self.rfile.read(n)
            self._not_found_probe()
            return

        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n) or b"{}")
        method = req.get("method", "")
        rid = req.get("id")

        if method == "initialize":
            self._sse({"jsonrpc": "2.0", "id": rid,
                       "result": {"protocolVersion": "2024-11-05",
                                  "capabilities": {},
                                  "serverInfo": {"name": "echo", "version": "0"}}})
        elif method.startswith("notifications/"):
            self.send_response(202)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif method == "tools/list":
            self._sse({"jsonrpc": "2.0", "id": rid, "result": {"tools": TOOLS}})
        elif method == "tools/call":
            text = req.get("params", {}).get("arguments", {}).get("text", "")
            self._sse({"jsonrpc": "2.0", "id": rid,
                       "result": {"content": [{"type": "text", "text": text}],
                                  "isError": False}})
        else:
            self._sse({"jsonrpc": "2.0", "id": rid,
                       "error": {"code": -32601, "message": "no such method"}})


def main():
    srv = HTTPServer(("127.0.0.1", 0), Handler)
    print(f"PORT {srv.server_address[1]}", flush=True)

    def _stop(signum, frame):
        # signal handlers must not touch the socket directly; just flip a
        # flag serve_forever polls, then let it unwind on its own thread.
        threading.Thread(target=srv.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, _stop)
    srv.serve_forever()


if __name__ == "__main__":
    main()
