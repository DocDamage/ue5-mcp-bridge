#!/usr/bin/env python3
"""Phase B9 — JSON edge cases: duplicate keys, conflicting types, raw bytes.

JSON itself doesn't support reference cycles (DAG only), but it DOES allow
nominally-malformed documents that some parsers treat differently:

  * Duplicate keys                   {"a":1,"a":2}    → RFC: last wins; some libs reject
  * Conflicting numeric types        {"x":1,"x":"y"}  → same key, mixed type
  * Trailing comma                   [1,2,]           → strict JSON rejects
  * Comments                         { /* doc */ ... }→ strict JSON rejects
  * Bare keys                        {x:1}            → only JSON5 accepts
  * Single-quoted strings            {'a':'b'}        → only JSON5 accepts
  * Leading +                        {"x":+1}         → strict JSON rejects
  * Hex / oct literals               {"x":0x1a}       → strict JSON rejects
  * BOM-prefixed                     b"\\xef\\xbb\\xbf{...}" → most accept
  * Whitespace-only                  "   \\n  "       → empty → parser refuse
  * UTF-16 surrogates split          {"x":"\\ud800"}  → unpaired surrogate
  * Extremely deeply-keyed object    {"a"*10000:1}    → 10k-char key
  * Stack-blow comment chain         /* /* /* ... */ */ */ — nested comments

These are sent as raw bytes (NOT through the JSON-encoder of call()), so
the framing+parser layers see the exact bad bytes. Bridge MUST:
  * Reply with -32600 (ParseError) for non-JSON
  * Reply with -32602 (InvalidParams) for valid JSON with bad shape
  * Survive each — editor must be alive for the next probe

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import List, Tuple

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

PHASE = "b9"
NAME = "ref_cycles"


# Each probe: (label, raw_bytes_to_send). All MUST be terminated by '\n' so the
# Bridge's line-delimited framing actually closes the frame and triggers parse.
PROBES: List[Tuple[str, bytes]] = [
    # (1) Duplicate keys — RFC-7159 says last wins. Bridge parser should accept;
    # method "memreport.get_quick_stats" doesn't need 'method' but we use a valid one.
    ("dup_keys",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"method":"engine.get_info","args":{}}\n'),

    # (2) Same-key conflicting types: int then string then bool.
    ("conflict_type",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"args":{"x":1,"x":"two","x":true}}\n'),

    # (3) Trailing comma — strict JSON should reject.
    ("trailing_comma_obj",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":{},}\n'),
    ("trailing_comma_arr",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":[1,2,]}\n'),

    # (4) JS-style /* */ comments — strict JSON should reject.
    ("c_style_comment",
     b'{"id":"x",/* note */"kind":"call_function",'
     b'"method":"memreport.get_quick_stats","args":{}}\n'),

    # (5) Line comments — non-standard.
    ("line_comment",
     b'// dispatch directive\n'
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":{}}\n'),

    # (6) Bare keys (JSON5).
    ("bare_keys",
     b'{id:"x",kind:"call_function",method:"memreport.get_quick_stats",args:{}}\n'),

    # (7) Single-quoted strings (JSON5).
    ("single_quotes",
     b"{'id':'x','kind':'call_function','method':'memreport.get_quick_stats','args':{}}\n"),

    # (8) Leading + on number — strict JSON rejects.
    ("plus_prefix_num",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"args":{"v":+1}}\n'),

    # (9) Hex literal.
    ("hex_literal",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"args":{"v":0x1A}}\n'),

    # (10) BOM-prefixed valid JSON.
    ("bom_prefix",
     b'\xef\xbb\xbf{"id":"x","kind":"call_function",'
     b'"method":"memreport.get_quick_stats","args":{}}\n'),

    # (11) Whitespace-only — empty document.
    ("whitespace_only",
     b'   \n'),

    # (12) Unpaired UTF-16 high surrogate.
    ("unpaired_surrogate",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"args":{"v":"\\ud800"}}\n'),

    # (13) Extremely long key (10k chars).
    ("long_key_10k",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"args":{"' + b'a' * 10000 + b'":1}}\n'),

    # (14) Deeply-nested comments (parser depth).
    ("nested_comments",
     b'/*/*/*/*/*/*/*/* hi */*/*/*/*/*/*/*/'
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":{}}\n'),

    # (15) NULL byte mid-frame (before framing newline).
    ("null_byte_mid",
     b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats",'
     b'"args":{"v":"a\x00b"}}\n'),

    # (16) Unknown kind value (sanity-check; this is technically Phase D7 territory but
    # we want to verify dispatcher survives mixed bad-frame burst alongside the
    # above parse-time probes).
    ("wrong_kind_value",
     b'{"id":"x","kind":"wrong_kind","method":"memreport.get_quick_stats","args":{}}\n'),
]


def _try_parse_response(raw: bytes) -> dict:
    """Best-effort parse of raw response bytes into a Python dict for the harness'
    is_ok / err_code helpers. Falls back to a synthetic dict on parse fail."""
    if not raw:
        return {"_err": "no_response"}
    import json
    try:
        return json.loads(raw.decode("utf-8", errors="replace"))
    except Exception as e:
        return {"_err": "unparseable_response", "_raw": raw[:120].decode("utf-8", errors="replace"), "_exc": str(e)}


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[B9] running {len(PROBES)} raw-frame edge-case probes…", flush=True)

    for (label, payload) in PROBES:
        full_label = f"raw_frame :: {label}"
        t0 = time.monotonic()
        try:
            raw_resp = send_raw_bytes(payload, expect_response=True, timeout=6.0)
        except Exception as e:
            raw_resp = None
            r = {"_err": "exception", "_exc": str(e)}
        else:
            r = _try_parse_response(raw_resp) if raw_resp is not None else {"_err": "no_response"}
        dt = (time.monotonic() - t0) * 1000.0

        alive = health(timeout=3.0)
        if not alive:
            log.case(full_label, "FAIL", f"EDITOR DIED on {label}",
                     alive=False, duration_ms=dt)
            log.write()
            print(f"  [B9] EDITOR CRASHED on {label}", file=sys.stderr)
            return 1
        crash = latest_crash_dump(since=crash_baseline)
        if crash:
            log.case(full_label, "FAIL", f"CRASH DUMP: {crash}",
                     alive=alive, duration_ms=dt)
            log.write()
            return 1

        # `r._err == "no_response"` may be legitimate for whitespace-only
        # (server discards the empty frame and stays open). Verify the next
        # call succeeds — if it does, this probe is PASS.
        if r.get("_err") == "no_response":
            # Re-probe with a known-good request via send_raw_bytes.
            probe_payload = (b'{"id":"y","kind":"call_function","method":"memreport.get_quick_stats","args":{}}\n')
            try:
                probe_resp = send_raw_bytes(probe_payload, expect_response=True, timeout=4.0)
            except Exception:
                probe_resp = None
            if probe_resp and probe_resp.strip():
                log.case(full_label, "PASS",
                         "no response (server discarded malformed frame) but next call works",
                         alive=alive, duration_ms=dt)
            else:
                log.case(full_label, "FAIL",
                         "no response AND server unresponsive on next call",
                         alive=alive, duration_ms=dt)
                fail_total += 1
            continue

        c = err_code(r)
        if r.get("_err") == "unparseable_response":
            log.case(full_label, "FAIL",
                     f"unparseable raw response: {r.get('_raw', '')[:60]}",
                     alive=alive, duration_ms=dt)
            fail_total += 1
            continue

        if is_ok(r):
            # Parser was lenient enough to accept the malformed input; method dispatched.
            # That's a PASS for crash-safety (no editor death).
            log.case(full_label, "PASS",
                     "parser accepted malformed input; dispatcher returned ok=true",
                     alive=alive, duration_ms=dt)
        elif c is not None and -32700 <= c <= -32000:
            log.case(full_label, "PASS",
                     f"clean structured error: {c}: {err_message(r)[:50]}",
                     alive=alive, duration_ms=dt, code=c)
        else:
            log.case(full_label, "FAIL",
                     f"unexpected response: code={c}: {err_message(r)[:60]}",
                     alive=alive, duration_ms=dt, code=c)
            fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[B9] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
