#!/usr/bin/env python3
"""Phase B8 — JSON recursion / depth bomb.

Goal: deeply nested JSON in args must not blow the dispatcher stack.
UE's TJsonReaderFactory uses recursive descent; UE 5.x ships with a
reasonable default stack but if our framing layer pre-parses JSON
twice (once to find the kind/method, once to deserialize args), the
combined depth could exhaust the worker thread's stack.

Probes:
  * Pure object nesting   { "a": { "a": { ... 500 } } }
  * Pure array nesting    [[[[[...500]]]]]
  * Mixed nesting         { "a": [ { "a": [ ... 200/200 ] } ] }
  * Wide arrays           [0, 1, 2, ..., 10000]    (single-level)
  * Wide objects          { "k0": 0, "k1": 1, ..., k9999: 9999 }
  * Long string           "X" * 100_000 (single-field, single-value)

Each probe is delivered to a single arbitrary endpoint (`memreport.get_quick_stats`)
since the parser sits ABOVE the method dispatcher — bug exists at parse
time regardless of the method targeted. Using a Lane B method avoids
queueing the post-parse work.

PASS:
  * structured error -32600/-32602
  * ok=true (parser accepts; handler ignores the unknown field)
  * connection close with structured error before close

FAIL:
  * editor crash (assertion / stack-overflow / segfault)
  * unstructured timeout that leaves dispatcher wedged for next call

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from typing import Any, List, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    send_raw_bytes,
)

PHASE = "b8"
NAME = "depth_bomb"

# Aim for "deep but not gargantuan" — UE's JSON deserializer is tested on
# small docs; going above 1k starts to risk crashing UE's own log writers
# more than the dispatcher we're trying to harden. 500 = comfortable middle.
DEEP_LEVELS = 500
WIDE_COUNT = 10_000
HUGE_STRING_LEN = 100_000


def _make_deep_object(levels: int) -> Any:
    """{ "a": { "a": { ... }}} with `levels` keys deep."""
    out: Any = "leaf"
    for _ in range(levels):
        out = {"a": out}
    return out


def _make_deep_array(levels: int) -> Any:
    """[[[[ ... ]]]] with `levels` brackets deep."""
    out: Any = "leaf"
    for _ in range(levels):
        out = [out]
    return out


def _make_mixed(levels_each: int) -> Any:
    """{ a: [ { a: [ ... ] } ] }."""
    out: Any = "leaf"
    for _ in range(levels_each):
        out = [{"a": [out]}]
    return out


def _make_wide_array(count: int) -> Any:
    return list(range(count))


def _make_wide_object(count: int) -> Any:
    return {f"k{i}": i for i in range(count)}


PROBES: List[Tuple[str, Any]] = [
    (f"deep_object_{DEEP_LEVELS}", _make_deep_object(DEEP_LEVELS)),
    (f"deep_array_{DEEP_LEVELS}",  _make_deep_array(DEEP_LEVELS)),
    (f"mixed_{DEEP_LEVELS // 2}",   _make_mixed(DEEP_LEVELS // 2)),
    (f"wide_array_{WIDE_COUNT}",   _make_wide_array(WIDE_COUNT)),
    (f"wide_object_{WIDE_COUNT}",  _make_wide_object(WIDE_COUNT)),
    (f"huge_string_{HUGE_STRING_LEN}", "X" * HUGE_STRING_LEN),
]

# We pick a Lane B no-arg endpoint so probe collisions don't tie up Lane A.
TARGET_METHOD = "memreport.get_quick_stats"


def _send_via_call(args: Any) -> dict:
    """Standard call path — args is the args object."""
    return call(TARGET_METHOD, args if isinstance(args, dict) else {"payload": args}, timeout=20.0)


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[B8] {len(PROBES)} depth/width probes against {TARGET_METHOD}…", flush=True)

    for (label_suffix, payload) in PROBES:
        label = f"{TARGET_METHOD} :: {label_suffix}"
        t0 = time.monotonic()
        try:
            r = _send_via_call(payload)
        except Exception as e:
            r = {"_err": "exception", "_exc": str(e)}
        dt = (time.monotonic() - t0) * 1000.0
        c = err_code(r)
        alive = health(timeout=3.0)

        if not alive:
            log.case(label, "FAIL",
                     f"EDITOR DIED on probe {label_suffix}",
                     alive=False, duration_ms=dt)
            log.write()
            print(f"  [B8] EDITOR CRASHED on probe {label_suffix}",
                  file=sys.stderr)
            return 1
        crash = latest_crash_dump(since=crash_baseline)
        if crash:
            log.case(label, "FAIL", f"CRASH DUMP: {crash}",
                     alive=alive, duration_ms=dt, code=c)
            log.write()
            return 1
        if is_transport_failure(r):
            # Transport timeout is acceptable for huge payloads if subsequent
            # call works; check editor liveness and retry once.
            time.sleep(2.0)
            alive2 = health(timeout=5.0)
            if not alive2:
                log.case(label, "FAIL",
                         f"transport+ENGINE DOWN: {r.get('_err')}",
                         alive=False, duration_ms=dt)
                log.write()
                return 1
            log.case(label, "PASS",
                     f"transport timeout but editor recovered ({r.get('_err')})",
                     alive=alive2, duration_ms=dt)
            continue

        # Structured response — parser or dispatcher rejected cleanly.
        if is_ok(r):
            # Handler accepted the document and returned its normal output
            # (most likely it ignored the unknown payload field). PASS — parser
            # survived deep payload.
            log.case(label, "PASS",
                     "parser+dispatcher accepted deep payload (ok=true)",
                     alive=alive, duration_ms=dt)
        elif c is not None and -32700 <= c <= -32000:
            log.case(label, "PASS",
                     f"parser/dispatcher rejected cleanly: {c}: {err_message(r)[:50]}",
                     alive=alive, duration_ms=dt, code=c)
        else:
            log.case(label, "FAIL",
                     f"unexpected response: code={c}: {err_message(r)[:60]}",
                     alive=alive, duration_ms=dt, code=c)
            fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[B8] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
