#!/usr/bin/env python3
"""End-to-end smoke test for the MCP bridge TCP listener (Phase 1 Day 2).

Prerequisites:
  * Unreal Editor must be running with the UnrealMCPBridge plugin loaded.
  * Look for log line: `LogMCP: MCP bridge listening on 127.0.0.1:30020`.

What this script does:
  1. Connect to 127.0.0.1:30020 (newline-framed JSON wire format).
  2. Send a single ``editor.ping`` request: ``{"id":"test-1","kind":"call_function","method":"editor.ping","args":{}}\\n``
  3. Wait up to 5 seconds for a response line.
  4. Assert ``{"id":"test-1","ok":true,"result":{"pong":true}}``.
  5. Print ``[SMOKE_PING] PASS`` or ``[SMOKE_PING] FAIL reason=...`` and exit 0 / 1.

Usage:
  python smoke_ping.py [--host HOST] [--port PORT] [--id ID]
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
DEFAULT_ID = "test-1"
READ_TIMEOUT_SEC = 5.0


def send_and_recv_line(host: str, port: int, request_obj: dict) -> Optional[dict]:
    """Open a fresh TCP socket, send one newline-framed JSON request, read one response line.

    Returns the parsed response dict, or None on timeout / connection error.
    """

    with socket.create_connection((host, port), timeout=READ_TIMEOUT_SEC) as sock:
        sock.settimeout(READ_TIMEOUT_SEC)
        payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
        sock.sendall(payload)

        # Read until we get a newline. Bridge sends exactly one response line per request.
        buf = bytearray()
        deadline = time.monotonic() + READ_TIMEOUT_SEC
        while True:
            if time.monotonic() > deadline:
                return None
            try:
                chunk = sock.recv(64 * 1024)
            except socket.timeout:
                return None
            if not chunk:
                # Peer closed before sending newline.
                break
            buf.extend(chunk)
            newline_idx = buf.find(b"\n")
            if newline_idx >= 0:
                line = bytes(buf[:newline_idx])
                return json.loads(line.decode("utf-8"))

        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--id", default=DEFAULT_ID, help="request id to echo back (opaque, non-GUID is fine)")
    args = parser.parse_args()

    request = {
        "id": args.id,
        "kind": "call_function",
        "method": "editor.ping",
        "args": {},
    }

    print(f"[SMOKE_PING] connecting to {args.host}:{args.port} ...")
    try:
        response = send_and_recv_line(args.host, args.port, request)
    except (ConnectionRefusedError, OSError) as exc:
        print(f"[SMOKE_PING] FAIL reason=connect-error detail={exc}")
        return 1
    except json.JSONDecodeError as exc:
        print(f"[SMOKE_PING] FAIL reason=invalid-json-response detail={exc}")
        return 1

    if response is None:
        print(f"[SMOKE_PING] FAIL reason=timeout-waiting-for-response (>{READ_TIMEOUT_SEC}s)")
        return 1

    # Verify required shape.
    expected_id = args.id
    if response.get("id") != expected_id:
        print(f"[SMOKE_PING] FAIL reason=id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return 1
    if response.get("ok") is not True:
        print(f"[SMOKE_PING] FAIL reason=ok-not-true got={response.get('ok')!r} error={response.get('error')!r}")
        return 1
    result = response.get("result")
    if not isinstance(result, dict) or result.get("pong") is not True:
        print(f"[SMOKE_PING] FAIL reason=pong-not-true result={result!r}")
        return 1

    print(f"[SMOKE_PING] PASS response={response}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
