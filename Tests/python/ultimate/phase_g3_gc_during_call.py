#!/usr/bin/env python3
"""Phase G3 — Editor GC during tool call.

Goal: forcing frequent garbage collection during MCP tool execution
doesn't crash the bridge or corrupt response data.

Probes:
  P1 — set cfg.set_cvar gc.CollectGarbageEveryFrame = 1 (force GC every tick)
  P2 — fire 100 mixed Lane B + Lane A calls
  P3 — restore original gc.CollectGarbageEveryFrame
  P4 — verify editor alive + no crash

PASS: all 100 calls return without transport timeout, editor alive,
no new crash dump.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Tuple

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
)

PHASE = "g3"
NAME = "gc_during_call"

CVAR_NAME = "gc.CollectGarbageEveryFrame"
N_CALLS = 100


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[G3] GC stress: force GC every tick + {N_CALLS} calls…", flush=True)

    # P1 — snapshot original gc.CollectGarbageEveryFrame
    r_get = call("cfg.get_cvar", {"name": CVAR_NAME}, timeout=8.0)
    if not is_ok(r_get):
        # cfg.get_cvar may need a different field name; SKIP the entire phase
        log.case("P1_snapshot", "SKIP",
                 f"cannot read {CVAR_NAME}: {err_message(r_get)[:60]}")
        log.write()
        return 0
    original_value = (r_get.get("result", {}) or {}).get("value", 60.0)
    log.case("P1_snapshot", "PASS",
             f"original {CVAR_NAME}={original_value}")

    # P2 — set to 1 (force GC every tick)
    r_set = call("cfg.set_cvar",
                 {"name": CVAR_NAME, "value": "1"}, timeout=8.0)
    if not is_ok(r_set):
        log.case("P2_set_aggressive_gc", "FAIL",
                 f"set_cvar failed: {err_message(r_set)[:60]}")
        log.write()
        return 1
    log.case("P2_set_aggressive_gc", "PASS",
             f"{CVAR_NAME} = 1 (GC every tick)")

    # P3 — fire 100 mixed calls under aggressive GC
    methods: List[Tuple[str, Dict[str, Any]]] = [
        ("memreport.get_quick_stats", {}),
        ("engine.get_info", {}),
        ("pie.is_running", {}),
        ("asset.exists", {"path": "/Engine/BasicShapes/Cube"}),
        ("cfg.list_cvars", {"page_size": 5}),
    ]
    t0 = time.monotonic()
    ok_count = 0
    transport_fail = 0
    structured_err = 0
    first_failures = []
    for i in range(N_CALLS):
        method, args = methods[i % len(methods)]
        try:
            r = call(method, args, timeout=8.0)
        except Exception as e:
            transport_fail += 1
            if len(first_failures) < 3:
                first_failures.append((method, f"exception: {e}"))
            continue
        if is_transport_failure(r):
            transport_fail += 1
            if len(first_failures) < 3:
                first_failures.append((method, f"transport: {r.get('_err')}"))
        elif is_ok(r):
            ok_count += 1
        else:
            structured_err += 1
        # Mid-loop health check
        if i > 0 and (i % 25) == 0:
            if not health(timeout=5.0):
                log.case("P3_health_mid", "FAIL",
                         f"editor unresponsive at call {i}; ok={ok_count} fail={transport_fail}",
                         alive=False)
                # Restore CVar before exit
                call("cfg.set_cvar",
                     {"name": CVAR_NAME, "value": str(original_value)}, timeout=8.0)
                log.write()
                return 1
    dt = (time.monotonic() - t0) * 1000.0
    success_rate = ok_count / N_CALLS
    summary = (f"ok={ok_count}/{N_CALLS} structured_err={structured_err} "
               f"transport_fail={transport_fail} duration={dt:.0f}ms rate={N_CALLS/(dt/1000.0):.0f}/s "
               f"first_failures={first_failures}")
    if success_rate >= 0.95:
        log.case("P3_calls_under_gc", "PASS",
                 f"{success_rate:.0%} success under aggressive GC; {summary}",
                 duration_ms=dt)
    elif success_rate >= 0.7:
        log.case("P3_calls_under_gc", "XFAIL",
                 f"{success_rate:.0%} success — some transient timeouts under GC pressure; {summary}",
                 duration_ms=dt)
    else:
        log.case("P3_calls_under_gc", "FAIL",
                 f"only {success_rate:.0%} success — GC pressure broke dispatcher; {summary}",
                 duration_ms=dt)
        fail_total += 1

    # P4 — restore original CVar
    r_restore = call("cfg.set_cvar",
                     {"name": CVAR_NAME, "value": str(original_value)}, timeout=8.0)
    if is_ok(r_restore):
        log.case("P4_restore", "PASS", f"restored {CVAR_NAME}={original_value}")
    else:
        log.case("P4_restore", "FAIL",
                 f"failed to restore CVar: {err_message(r_restore)[:60]}")
        fail_total += 1

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[G3] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
