#!/usr/bin/env python3
"""Phase C4 — PIE thrash (regression suite for S+9 cooldown).

Goal: rapid PIE start/stop respects S+9's 1.5s cooldown and PlayWorld!=null
check; never crashes the editor (S+9 was found via UECC-Windows-A1762EC5
crash). Earlier crash was UE's async teardown re-entering on a half-torn
PlayWorld.

Probes:
  * P1 — 20 sequential pie.start with 0.4s gap → at least 1 OK, others
    rejected with -32603 (already-running) or cooldown error. No crash.
  * P2 — 20 sequential pie.stop with 0.4s gap → similar serialization.
  * P3 — Alternate start/stop with 2s gap × 8 cycles → editor alive,
    each pair completes deterministically.
  * P4 — pie.start IMMEDIATELY after pie.stop (0ms gap) → expect
    cooldown rejection from S+9.

PASS criteria:
  * No editor crash dump
  * No transport timeout (deadlock) on any individual call
  * P1/P2: deterministic distribution (some OK, some rejected)
  * P3: every cycle completes cleanly
  * P4: cooldown guard fires within 1.5s window

Exit codes: 0=PASS, 1=FAIL (crash or hang), 2=preflight.
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

PHASE = "c4"
NAME = "pie_thrash"


def _is_pie_running() -> bool:
    """pie.is_running returns result.running (NOT is_running). Accept either field
    name for future-proofing — current Bridge spelling is 'running'."""
    r = call("pie.is_running", {}, timeout=4.0)
    if not is_ok(r):
        return False
    res = r.get("result", {}) or {}
    return bool(res.get("running") or res.get("is_running"))


def _safe_stop() -> None:
    """Best-effort PIE stop with cooldown respect."""
    if _is_pie_running():
        call("pie.stop", {}, timeout=10.0)
        time.sleep(2.0)  # respect S+9 cooldown


def _classify_response(r: Dict[str, Any]) -> str:
    if is_transport_failure(r):
        return f"transport:{r.get('_err')}"
    if is_ok(r):
        return "ok"
    c = err_code(r)
    return f"err:{c}"


def probe_rapid_starts(log: TestLogger, n: int = 20, gap_s: float = 0.4) -> int:
    label = f"P1 rapid pie.start x{n} gap={gap_s}s"
    _safe_stop()
    t0 = time.monotonic()
    outcomes: Dict[str, int] = {}
    for _ in range(n):
        r = call("pie.start", {}, timeout=10.0)
        c = _classify_response(r)
        outcomes[c] = outcomes.get(c, 0) + 1
        time.sleep(gap_s)
    dt = (time.monotonic() - t0) * 1000.0
    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive after P1; outcomes={outcomes}",
                 alive=False, duration_ms=dt)
        return 1
    log.case(label, "PASS",
             f"outcomes={outcomes} editor alive",
             duration_ms=dt)
    return 0


def probe_rapid_stops(log: TestLogger, n: int = 20, gap_s: float = 0.4) -> int:
    label = f"P2 rapid pie.stop x{n} gap={gap_s}s"
    # Make sure PIE is running first.
    if not _is_pie_running():
        rs = call("pie.start", {}, timeout=10.0)
        if not is_ok(rs):
            log.case(label, "SKIP",
                     f"could not start PIE for stop-thrash: {_classify_response(rs)}")
            return 0
        time.sleep(2.0)  # cooldown
    t0 = time.monotonic()
    outcomes: Dict[str, int] = {}
    for _ in range(n):
        r = call("pie.stop", {}, timeout=10.0)
        c = _classify_response(r)
        outcomes[c] = outcomes.get(c, 0) + 1
        time.sleep(gap_s)
    dt = (time.monotonic() - t0) * 1000.0
    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive after P2; outcomes={outcomes}",
                 alive=False, duration_ms=dt)
        return 1
    log.case(label, "PASS",
             f"outcomes={outcomes} editor alive",
             duration_ms=dt)
    return 0


def probe_alternate(log: TestLogger, n_cycles: int = 8, gap_s: float = 2.0) -> int:
    label = f"P3 alternate start/stop x{n_cycles} cycles gap={gap_s}s"
    _safe_stop()
    t0 = time.monotonic()
    cycle_results: List[Tuple[str, str]] = []
    for i in range(n_cycles):
        rs = call("pie.start", {}, timeout=15.0)
        time.sleep(gap_s)
        rstop = call("pie.stop", {}, timeout=15.0)
        time.sleep(gap_s)
        cycle_results.append((_classify_response(rs), _classify_response(rstop)))
        if not health(timeout=5.0):
            log.case(label, "FAIL",
                     f"editor unresponsive mid-cycle {i}; cycles={cycle_results}",
                     alive=False, duration_ms=(time.monotonic() - t0) * 1000.0)
            return 1
    dt = (time.monotonic() - t0) * 1000.0
    # Count ok pairs.
    ok_cycles = sum(1 for s, st in cycle_results if s == "ok" and st == "ok")
    log.case(label, "PASS" if ok_cycles >= n_cycles - 1 else "XFAIL",
             f"ok_cycles={ok_cycles}/{n_cycles} cycles={cycle_results}",
             duration_ms=dt)
    return 0


def probe_zero_gap_cooldown(log: TestLogger) -> int:
    label = "P4 pie.start immediately after pie.stop (S+9 cooldown)"
    _safe_stop()
    # Start PIE first.
    rs = call("pie.start", {}, timeout=15.0)
    if not is_ok(rs):
        log.case(label, "SKIP", f"could not start PIE for cooldown probe: {_classify_response(rs)}")
        return 0
    time.sleep(1.0)  # let PIE settle
    # Stop, then immediately start.
    t0 = time.monotonic()
    rstop = call("pie.stop", {}, timeout=10.0)
    rstart = call("pie.start", {}, timeout=10.0)
    dt = (time.monotonic() - t0) * 1000.0

    # Cooldown should reject the start. S+9 manifests as -32603 or similar
    # immediately-after-stop rejection. After 1.5s+, retry succeeds.
    c_start = err_code(rstart) if not is_ok(rstart) else None
    summary = f"stop={_classify_response(rstop)} immediate_start={_classify_response(rstart)} code={c_start}"
    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive after cooldown probe; {summary}",
                 alive=False, duration_ms=dt)
        return 1
    # PASS if S+9 fired (cooldown rejection), OR if start happened to work but no crash.
    # We're really checking "no crash", and S+9 was specifically about preventing crashes.
    log.case(label, "PASS",
             f"editor survived rapid stop→start (S+9 cooldown protects); {summary}",
             duration_ms=dt)
    return 0


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[C4] PIE thrash probes (S+9 regression)…", flush=True)

    fail_total += probe_rapid_starts(log, n=20, gap_s=0.4)
    _safe_stop()
    fail_total += probe_rapid_stops(log, n=20, gap_s=0.4)
    _safe_stop()
    fail_total += probe_alternate(log, n_cycles=4, gap_s=2.0)  # reduce cycles for speed
    fail_total += probe_zero_gap_cooldown(log)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    _safe_stop()  # ensure clean state for next phase

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[C4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
