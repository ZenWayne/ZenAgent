#!/usr/bin/env python3
"""Trivial MCP server over stdio for integration tests.

Implements the three MCP methods agentflow uses (`initialize`, `tools/list`,
`tools/call`) plus a single tool `echo` that returns whatever you pass in
`arguments.text`. Newline-delimited JSON-RPC, no streaming.
"""

import json
import sys


def reply(req_id, *, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": req_id}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def main():
    # NOTE: `for line in sys.stdin` (and sys.stdin.readline()) block-buffer
    # because sys.stdin is a TextIOWrapper. Read raw bytes via stdin.buffer
    # so each newline-delimited request is delivered as soon as it arrives.
    stdin = sys.stdin.buffer
    while True:
        raw = stdin.readline()
        if not raw:
            return  # EOF
        line = raw.decode("utf-8").strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        method = msg.get("method")
        req_id = msg.get("id")
        if method == "initialize":
            reply(
                req_id,
                result={
                    "protocolVersion": "2025-03-26",
                    "capabilities": {},
                    "serverInfo": {"name": "echo", "version": "0.1"},
                },
            )
        elif method == "tools/list":
            reply(
                req_id,
                result={
                    "tools": [
                        {
                            "name": "echo",
                            "description": "Echo back the `text` argument.",
                            "inputSchema": {
                                "type": "object",
                                "properties": {"text": {"type": "string"}},
                                "required": ["text"],
                            },
                        }
                    ]
                },
            )
        elif method == "tools/call":
            params = msg.get("params", {})
            name = params.get("name")
            args = params.get("arguments", {}) or {}
            if name == "echo":
                text = args.get("text", "")
                reply(
                    req_id,
                    result={"content": [{"type": "text", "text": text}]},
                )
            else:
                reply(
                    req_id,
                    error={"code": -32601, "message": f"unknown tool: {name}"},
                )
        elif method and method.startswith("notifications/"):
            # No reply for notifications. Ignore.
            continue
        elif req_id is not None:
            reply(
                req_id,
                error={"code": -32601, "message": f"unimplemented: {method}"},
            )


if __name__ == "__main__":
    main()
