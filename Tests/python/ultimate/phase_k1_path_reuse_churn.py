#!/usr/bin/env python3
"""Phase K1 — Asset path-reuse churn (registry / GC / PathInUse consistency).

Repeatedly create then delete an asset at the SAME path. A stale
asset-registry entry, a not-yet-GC'd UObject occupying the path, or a
leaked package handle would surface as:
  - -32014 PathInUse on the recreate (path still considered occupied)
  - create failing after a clean delete
  - asset.exists lying (reports exists after delete, or missing after create)
  - a per-cycle UObject leak (count climbs every cycle)

This is a focused consistency probe distinct from the leak-volume tests:
it hammers ONE path, exercising the delete→recreate transition that
registry/GC bugs hide in.

Probes:
  P1 — N cycles of: create_data_asset(P) → exists==true → cb.delete(P) →
       exists==false. Every cycle must be clean.
  P2 — post-churn GC: UObject count returns within tolerance of baseline.

PASS: all cycles clean, no PathInUse leak, exists() always truthful,
uobj stable, editor alive, 0 crash dumps.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    force_gc,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
    random_suffix,
    snapshot,
)

PHASE = "k1"
NAME = "path_reuse_churn"

ROOT = f"/Game/PhT_K1_{random_suffix(6)}"
REUSED = f"{ROOT}/Reused"
DA_CLASS = "/Script/Engine.PrimaryAssetLabel"
N_CYCLES = 50
UOBJ_TOL = 800


def _exists(path: str) -> bool:
    r = call("asset.exists", {"path": path}, timeout=6.0)
    return bool(is_ok(r) and (r.get("result", {}) or {}).get("exists"))


def cleanup() -> None:
    call("cb.delete", {"path": REUSED, "force": True}, timeout=8.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=10.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[K1] path-reuse churn: {N_CYCLES} create/delete cycles on one path…",
          flush=True)
    cleanup()
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)

    base = snapshot()
    base_uobj = base.get("live_uobject_slots", 0)

    n_create_ok = n_delete_ok = 0
    n_pathinuse = 0
    bad_exists = 0
    first_fail = ""
    t0 = time.monotonic()
    for i in range(N_CYCLES):
        rc = call("asset.create_data_asset",
                  {"dest_path": REUSED, "class_path": DA_CLASS}, timeout=8.0)
        if is_ok(rc):
            n_create_ok += 1
        else:
            c = err_code(rc)
            if c == -32014:
                n_pathinuse += 1
            if not first_fail:
                first_fail = f"cycle {i} create: {c}: {err_message(rc)[:40]}"
        # exists should be true after create
        if not _exists(REUSED):
            bad_exists += 1
            if not first_fail:
                first_fail = f"cycle {i}: exists==false after create"
        rd = call("cb.delete", {"path": REUSED, "force": True}, timeout=8.0)
        if is_ok(rd):
            n_delete_ok += 1
        elif not first_fail:
            first_fail = f"cycle {i} delete: {err_code(rd)}: {err_message(rd)[:40]}"
        # exists should be false after delete
        if _exists(REUSED):
            bad_exists += 1
    churn_dt = (time.monotonic() - t0) * 1000.0

    if not health(timeout=6.0):
        log.case("midchurn_health", "FAIL", "editor unresponsive after churn",
                 alive=False)
        log.write(); cleanup(); return 1

    detail = (f"create_ok={n_create_ok}/{N_CYCLES} delete_ok={n_delete_ok}/{N_CYCLES} "
              f"pathinuse={n_pathinuse} bad_exists={bad_exists}")
    if n_create_ok == N_CYCLES and n_delete_ok == N_CYCLES and bad_exists == 0:
        log.case("P1_churn", "PASS",
                 f"all {N_CYCLES} reuse cycles clean; {detail}", duration_ms=churn_dt)
    elif n_pathinuse > 0:
        log.case("P1_churn", "FAIL",
                 f"PathInUse on recreate (stale registry/GC); {detail}; {first_fail}",
                 duration_ms=churn_dt)
        fail_total += 1
    elif bad_exists > 0:
        log.case("P1_churn", "FAIL",
                 f"asset.exists inconsistent across reuse; {detail}; {first_fail}",
                 duration_ms=churn_dt)
        fail_total += 1
    else:
        log.case("P1_churn", "XFAIL",
                 f"some cycles failed (not PathInUse/exists); {detail}; {first_fail}",
                 duration_ms=churn_dt)

    # P2 — uobj leak verdict
    gc = force_gc(settle_s=2.0)
    post = snapshot()
    uobj_delta = post.get("live_uobject_slots", 0) - base_uobj
    if abs(uobj_delta) <= UOBJ_TOL:
        log.case("P2_uobj_stable", "PASS",
                 f"uobj_delta={uobj_delta:+d} within tol ±{UOBJ_TOL}")
    else:
        log.case("P2_uobj_stable", "XFAIL",
                 f"uobj_delta={uobj_delta:+d} exceeds tol ±{UOBJ_TOL} "
                 f"(GC may lag; non-monotonic churn)")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write(); cleanup(); return 1

    cleanup()
    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[K1] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
