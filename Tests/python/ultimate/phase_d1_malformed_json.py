#!/usr/bin/env python3
"""Phase D1 — Malformed JSON / protocol layer.

Goal: parser refuses bad JSON cleanly with -32600 (ParseError) or -32602
(InvalidParams) per JSON-RPC convention, doesn't drop connection unsolicited,
doesn't crash the dispatcher.

Probes (sent as raw bytes via send_raw_bytes — bypasses harness JSON encoder):
  1.  "not json\\n"                          → -32600 ParseError
  2.  "{\\n"  (truncated)                    → -32600 (or close timeout)
  3.  "{}\\n" (no method)                    → -32600/-32602 (missing 'method')
  4.  args=null (where object expected)      → -32602
  5.  extra_field=...                        → ignored, normal dispatch
  6.  wrong kind="wrong"                     → -32600 or -32601
  7.  JSON with literal \\n inside string    → parsed correctly
  8.  Three valid frames pipelined           → all three dispatch in order
  9.  Empty frame "\\n"                     → ignored or -32600
  10. UTF-8 with BOM                         → accepted or clean error
  11. Very long valid frame (100KB)          → handled or capped

PASS: each probe returns a structured response (or empty if frame silently
dropped) AND a SUBSEQUENT valid call succeeds — connection didn't get wedged.

FAIL: editor crash OR any probe wedges the dispatcher for next call.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import json
import socket
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    send_raw_bytes,
)

PHASE = "d1"
NAME = "malformed_json"


def _parse(raw: Optional[bytes]) -> dict:
    if raw is None:
        return {"_err": "no_response"}
    if not raw.strip():
        return {"_err": "empty"}
    try:
        return json.loads(raw.decode("utf-8", errors="replace"))
    except Exception as e:
        return {"_err": "unparseable", "_raw": raw[:60].decode("utf-8", errors="replace"), "_exc": str(e)}


# Each probe: (label, raw_bytes, expected outcomes)
PROBES: List[Tuple[str, bytes]] = [
    ("not_json",
     b"not json\n"),
    ("truncated_brace",
     b"{\n"),
    ("empty_object_no_method",
     b'{}\n'),
    ("args_null",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":null}\n'),
    ("extra_field",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"args":{},"extra":"ignored"}\n'),
    ("wrong_kind",
     b'{"id":"x","kind":"wrong_kind","method":"memreport.get_quick_stats","args":{}}\n'),
    ("escaped_newline_in_string",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"args":{"note":"a\\nb"}}\n'),
    ("empty_frame",
     b"\n"),
    ("bom_prefix",
     b'\xef\xbb\xbf{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":{}}\n'),
    ("very_long_string_100k",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"args":{"v":"' + b'A' * 100_000 + b'"}}\n'),
    ("missing_id",
     b'{"kind":"call_function","method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_wrong_type",
     b'{"id":"x","kind":42,"method":"memreport.get_quick_stats","args":{}}\n'),
    ("method_wrong_type",
     b'{"id":"x","kind":"call_function","method":42,"args":{}}\n'),
    ("args_wrong_type_array",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":[1,2,3]}\n'),
]


def _probe_then_recover(label: str, payload: bytes, log: TestLogger,
                        crash_baseline: float) -> int:
    """Send malformed frame, parse response, verify subsequent valid call works.

    Returns: 0 = PASS, 1 = FAIL.
    """
    t0 = time.monotonic()
    try:
        raw_resp = send_raw_bytes(payload, expect_response=True, timeout=6.0)
    except Exception as e:
        raw_resp = None
        log.case(label, "FAIL", f"send exception: {e}",
                 duration_ms=(time.monotonic() - t0) * 1000.0)
        return 1
    r = _parse(raw_resp)
    dt = (time.monotonic() - t0) * 1000.0
    alive = health(timeout=3.0)
    if not alive:
        log.case(label, "FAIL", f"EDITOR DIED on {label}", alive=False, duration_ms=dt)
        return 1
    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case(label, "FAIL", f"CRASH DUMP: {crash}", alive=alive, duration_ms=dt)
        return 1

    # Verify dispatcher still works post-probe.
    probe_payload = (b'{"id":"y","kind":"call_function","method":"memreport.get_quick_stats","args":{}}\n')
    try:
        probe_resp = send_raw_bytes(probe_payload, expect_response=True, timeout=4.0)
    except Exception:
        probe_resp = None

    if probe_resp:
        try:
            probe_obj = json.loads(probe_resp.decode("utf-8", errors="replace"))
            if not probe_obj.get("ok"):
                log.case(label, "FAIL",
                         f"dispatcher wedged after probe: next-call returned {probe_obj}",
                         alive=alive, duration_ms=dt)
                return 1
        except Exception:
            log.case(label, "FAIL",
                     "dispatcher returned unparseable response on next call",
                     alive=alive, duration_ms=dt)
            return 1
    else:
        log.case(label, "FAIL",
                 "dispatcher dropped subsequent valid call (no response)",
                 alive=alive, duration_ms=dt)
        return 1

    # Classify probe response.
    if r.get("_err") in ("no_response", "empty"):
        # Server silently dropped frame — acceptable if dispatcher recovered (which it did).
        log.case(label, "PASS",
                 f"server silently dropped malformed frame (next call works)",
                 alive=alive, duration_ms=dt)
        return 0
    if r.get("_err") == "unparseable":
        log.case(label, "FAIL",
                 f"server response itself unparseable: {r.get('_raw', '')[:60]}",
                 alive=alive, duration_ms=dt)
        return 1

    c = err_code(r)
    if is_ok(r):
        log.case(label, "PASS", "server lenient — accepted malformed input, ok=true",
                 alive=alive, duration_ms=dt)
        return 0
    if c is not None and -32700 <= c <= -32000:
        log.case(label, "PASS",
                 f"clean structured error: {c}: {err_message(r)[:50]}",
                 alive=alive, duration_ms=dt, code=c)
        return 0
    log.case(label, "FAIL",
             f"unexpected response: code={c}: {err_message(r)[:60]}",
             alive=alive, duration_ms=dt, code=c)
    return 1


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[D1] running {len(PROBES)} malformed-JSON probes (each with post-probe recovery check)…", flush=True)

    for (label, payload) in PROBES:
        full_label = f"raw :: {label}"
        rc = _probe_then_recover(full_label, payload, log, crash_baseline)
        if rc != 0:
            fail_total += 1
            # Editor might be down. Continue if alive, abort if dead.
            if not health(timeout=5.0):
                log.write()
                return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[D1] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
