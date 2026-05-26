#!/usr/bin/env python3
"""Phase H6.6 — debug.* draw_line / sphere / box / flush coverage.

Goal: exercise the debug.* drawing surface end-to-end. These tools are
write-only (no read-back of drawn primitives — they live in the
rendering layer). PASS = each call returns ok=true with no editor crash.

Probes:
  P1 — debug.draw_line (start/end/color/thickness/duration)
  P2 — debug.draw_sphere (center/radius/color/segments/duration)
  P3 — debug.draw_box (center/extent/color/duration)
  P4 — debug.draw_string (location/text/color/duration)  (if exposed)
  P5 — debug.flush (clear all persistent debug draws)

Tools listed via Wave D Surface 2 — debug.* (6 tools per inventory).

Exit codes: 0=PASS, 1=FAIL (any), 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

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

PHASE = "h6_6"
NAME = "debug"


def _step(log: TestLogger, name: str, method: str, args: Dict[str, Any],
          timeout: float = 8.0) -> Optional[Dict[str, Any]]:
    t0 = time.monotonic()
    try:
        r = call(method, args, timeout=timeout)
    except Exception as e:
        log.case(name, "FAIL", f"exception: {e}",
                 duration_ms=(time.monotonic() - t0) * 1000.0)
        return None
    dt = (time.monotonic() - t0) * 1000.0
    if is_transport_failure(r):
        log.case(name, "FAIL", f"transport: {r.get('_err')}", duration_ms=dt)
        return None
    if not is_ok(r):
        c = err_code(r)
        # -32601 (tool not found) → SKIP (some debug.* may not be on this build)
        if c == -32601:
            log.case(name, "SKIP", f"{method} not registered", duration_ms=dt)
            return None
        log.case(name, "FAIL", f"{method}: code={c}: {err_message(r)[:60]}",
                 duration_ms=dt, code=c)
        return None
    log.case(name, "PASS", f"{method} ok", duration_ms=dt)
    return r.get("result", {}) or {}


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[H6.6] debug.* drawing surface…", flush=True)

    # P1 — debug.draw_line (vectors as [x,y,z] arrays per surface contract)
    _step(log, "P1_draw_line", "debug.draw_line",
          {"start": [0, 0, 0], "end": [100, 100, 100],
           "color": [1.0, 0.0, 0.0, 1.0],
           "thickness": 2.0, "duration": 1.0})

    # P2 — debug.draw_sphere
    _step(log, "P2_draw_sphere", "debug.draw_sphere",
          {"center": [100, 100, 100], "radius": 50.0,
           "color": [0.0, 1.0, 0.0, 1.0],
           "segments": 16, "duration": 1.0})

    # P3 — debug.draw_box
    _step(log, "P3_draw_box", "debug.draw_box",
          {"center": [200, 200, 200], "extent": [50, 50, 50],
           "color": [0.0, 0.0, 1.0, 1.0], "duration": 1.0})

    # P4 — debug.draw_string (may not exist)
    _step(log, "P4_draw_string", "debug.draw_string",
          {"location": [300, 300, 300], "text": "H6.6 marker",
           "color": [1.0, 1.0, 1.0, 1.0], "duration": 1.0})

    # P5 — debug.draw_arrow
    _step(log, "P5_draw_arrow", "debug.draw_arrow",
          {"start": [0, 0, 0], "end": [200, 0, 0],
           "color": [1.0, 1.0, 0.0, 1.0],
           "duration": 1.0, "arrow_size": 20.0})

    # P6 — debug.draw_circle
    _step(log, "P6_draw_circle", "debug.draw_circle",
          {"center": [400, 400, 400], "radius": 100.0,
           "color": [1.0, 0.0, 1.0, 1.0], "duration": 1.0})

    # P7 — debug.flush (clear all)
    _step(log, "P7_flush", "debug.flush", {})

    # Health + crash check
    if not health(timeout=5.0):
        log.case("final_health", "FAIL", "editor unresponsive after debug probes",
                 alive=False)
        log.write()
        return 1
    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H6.6] PASS={cc['PASS']} FAIL={cc['FAIL']} SKIP={cc.get('SKIP', 0)} "
          f"TOTAL={cc['TOTAL']}")
    print(f"       log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if cc["FAIL"] > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
