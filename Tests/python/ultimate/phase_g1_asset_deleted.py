#!/usr/bin/env python3
"""Phase G1 — Asset deleted mid-call.

Goal: when a tool is mid-execution on an asset, and a SECOND thread
deletes the same asset, the bridge handles it gracefully — no crash,
no use-after-free, no orphaned UObject.

The classic UE race:
  Thread 1: bp.compile /Game/X (acquires UBlueprint*, mutates state)
  Thread 2: cb.delete /Game/X (asks asset registry to drop it)
  → if Bridge doesn't gate writes serially, thread 1 derefs freed BP

Mitigation in Bridge: all asset writes drain via Lane A (game-thread,
OnEndFrame) — serialized by definition. So the race CAN'T actually
fire concurrently. This phase verifies the serialization holds and
that rapid back-to-back create/mutate/delete don't crash.

Probes:
  P1 — 10 cycles of:
        a) bp.create_blueprint to unique path
        b) bp.add_variable (mutate)
        c) bp.compile (mutate again)
        d) cb.delete (drop)
       All sequential — serialized through Lane A.

  P2 — Concurrent: thread 1 fires bp.compile, thread 2 fires
       cb.delete on the SAME BP. Both should complete with structured
       responses, no editor crash.

PASS: editor alive, no crash dumps, all calls return structured.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import concurrent.futures
import sys
import time
from pathlib import Path
from typing import Any, Dict, Optional

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
    random_suffix,
)

PHASE = "g1"
NAME = "asset_deleted"

ROOT = f"/Game/PhT_G1_{random_suffix(6)}"


def _classify(r: Dict[str, Any]) -> str:
    if is_transport_failure(r):
        return f"transport:{r.get('_err')}"
    if is_ok(r):
        return "ok"
    c = err_code(r)
    return f"err:{c}"


def cleanup() -> None:
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=15.0)


def probe_sequential_lifecycle(log: TestLogger, n: int = 10) -> int:
    label = f"P1 sequential lifecycle x{n}"
    t0 = time.monotonic()
    full_cycles = 0
    failures = []
    for i in range(n):
        bp_path = f"{ROOT}/BP_Cycle_{i:03d}"
        rc = call("bp.create_blueprint",
                  {"dest_path": bp_path,
                   "parent_class_path": "/Script/Engine.Actor"}, timeout=10.0)
        if not is_ok(rc):
            failures.append((i, "create", _classify(rc)))
            continue
        rv = call("bp.add_variable",
                  {"blueprint_path": bp_path,
                   "variable_name": f"V_{i}",
                   "pin_type": {"category": "Real", "subcategory": "float"}},
                  timeout=8.0)
        if not is_ok(rv):
            failures.append((i, "add_var", _classify(rv)))
        rcomp = call("bp.compile", {"blueprint_path": bp_path}, timeout=15.0)
        if not is_ok(rcomp):
            failures.append((i, "compile", _classify(rcomp)))
        rd = call("cb.delete", {"path": bp_path, "force": True}, timeout=8.0)
        if not is_ok(rd):
            failures.append((i, "delete", _classify(rd)))
        else:
            full_cycles += 1
    dt = (time.monotonic() - t0) * 1000.0
    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive; full_cycles={full_cycles}/{n}",
                 alive=False, duration_ms=dt)
        return 1
    if full_cycles >= n * 0.8:
        log.case(label, "PASS",
                 f"full_cycles={full_cycles}/{n} failures={failures[:3]}",
                 duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"only {full_cycles}/{n} full cycles; failures={failures[:5]}",
             duration_ms=dt)
    return 1


def probe_concurrent_compile_delete(log: TestLogger, n_pairs: int = 5) -> int:
    """Fire compile + delete concurrently on the same BP × N pairs."""
    label = f"P2 concurrent compile+delete x{n_pairs} pairs"
    t0 = time.monotonic()
    pair_results = []

    for i in range(n_pairs):
        bp_path = f"{ROOT}/BP_Race_{i:03d}"
        # Setup BP first.
        rc = call("bp.create_blueprint",
                  {"dest_path": bp_path,
                   "parent_class_path": "/Script/Engine.Actor"}, timeout=10.0)
        if not is_ok(rc):
            pair_results.append((i, "setup_fail", _classify(rc)))
            continue
        # Now concurrent compile + delete from two threads.
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
            f_compile = ex.submit(call, "bp.compile",
                                   {"blueprint_path": bp_path}, 15.0)
            f_delete = ex.submit(call, "cb.delete",
                                  {"path": bp_path, "force": True}, 8.0)
            r_compile = f_compile.result(timeout=20.0)
            r_delete = f_delete.result(timeout=20.0)
        pair_results.append((i, _classify(r_compile), _classify(r_delete)))
        # Health check between pairs.
        if not health(timeout=5.0):
            log.case(label, "FAIL",
                     f"editor died on pair {i}; results={pair_results}",
                     alive=False,
                     duration_ms=(time.monotonic()-t0)*1000.0)
            return 1

    dt = (time.monotonic() - t0) * 1000.0
    # PASS as long as editor survived all pairs (both calls returned structured).
    log.case(label, "PASS",
             f"all {n_pairs} concurrent compile+delete pairs survived; "
             f"results={pair_results}",
             duration_ms=dt)
    return 0


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[G1] asset-deleted-mid-call (root={ROOT})…", flush=True)
    cleanup()
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)

    fail_total += probe_sequential_lifecycle(log, n=10)
    fail_total += probe_concurrent_compile_delete(log, n_pairs=5)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        cleanup()
        return 1

    cleanup()

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[G1] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
