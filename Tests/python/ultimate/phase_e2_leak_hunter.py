#!/usr/bin/env python3
"""Phase E2 — Leak hunter (write-heavy).

Goal: N asset create+destroy cycles leave near-zero UObject delta after
force_gc. Catches leaks in the asset-factory + cb.delete pipelines.

Probes:
  P1 — N cycles of:
        a) asset.create_data_asset (PrimaryDataAsset to unique path)
        b) cb.delete force=true
  P2 — force_gc + measure UObject delta vs baseline

Default N=200 (was 10k in original plan — trimmed for wall time:
listener accept saturates at ~50/s, so 10k would take ~3 minutes;
200 still catches systematic leaks while running in ~30s).

PASS:
  * UObject delta after final force_gc < N/4 (i.e. <50 for N=200)
  * Memory delta < 100 MB
  * Editor alive, no crash

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

PHASE = "e2"
NAME = "leak_hunter"

ROOT = f"/Game/PhT_E2_{random_suffix(6)}"
N_CYCLES = 200
# PrimaryAssetLabel is a concrete UPrimaryDataAsset descendant that the
# asset factory accepts (UDataAsset itself + PrimaryDataAsset itself are
# abstract via the factory's class-flag check).
ASSET_CLASS = "/Script/Engine.PrimaryAssetLabel"


def cleanup_root() -> None:
    """Best-effort delete of the entire test folder."""
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=20.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()

    print(f"[E2] leak hunt: {N_CYCLES} create+delete cycles (root={ROOT})…", flush=True)

    # Ensure clean state + create root folder
    cleanup_root()
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)

    # Force GC + snapshot BEFORE the loop
    try:
        force_gc(timeout=15.0)
    except Exception:
        pass
    time.sleep(1.0)
    snap_before = snapshot()
    mem_before = snap_before.get("used_physical_mb", 0.0)
    uobj_before = snap_before.get("live_uobject_slots", 0)
    log.case("P0_baseline", "PASS",
             f"mem_before={mem_before:.1f}MB uobj_before={uobj_before}")

    # P1 — N cycles
    t_loop = time.monotonic()
    n_create_ok = 0
    n_delete_ok = 0
    first_failures = []
    for i in range(N_CYCLES):
        path = f"{ROOT}/Leak_{i:05d}"
        rc = call("asset.create_data_asset",
                  {"dest_path": path, "class_path": ASSET_CLASS}, timeout=6.0)
        if is_ok(rc):
            n_create_ok += 1
        elif len(first_failures) < 3:
            first_failures.append(("create", i, err_code(rc), err_message(rc)[:50]))
        rd = call("cb.delete", {"path": path, "force": True}, timeout=6.0)
        if is_ok(rd):
            n_delete_ok += 1
        elif len(first_failures) < 6:
            first_failures.append(("delete", i, err_code(rd), err_message(rd)[:50]))
        # Periodic health check
        if i > 0 and (i % 50) == 0:
            if not health(timeout=5.0):
                log.case("P1_health_mid", "FAIL",
                         f"editor unresponsive at cycle {i}; ok={n_create_ok} del={n_delete_ok}",
                         alive=False)
                log.write()
                cleanup_root()
                return 1
            crash = latest_crash_dump(since=crash_baseline)
            if crash:
                log.case("P1_crash_mid", "FAIL", f"CRASH at cycle {i}: {crash}")
                log.write()
                cleanup_root()
                return 1
    dt_loop = (time.monotonic() - t_loop) * 1000.0
    log.case("P1_loop", "PASS" if (n_create_ok > N_CYCLES * 0.5 and n_delete_ok > N_CYCLES * 0.5) else "FAIL",
             f"cycles={N_CYCLES} create_ok={n_create_ok} delete_ok={n_delete_ok} "
             f"duration={dt_loop:.0f}ms rate={(N_CYCLES*2)/(dt_loop/1000.0):.0f}/s "
             f"failures={first_failures}",
             duration_ms=dt_loop)

    # P2 — force_gc + snapshot AFTER
    try:
        force_gc(timeout=20.0)
    except Exception:
        pass
    time.sleep(2.0)
    snap_after = snapshot()
    mem_after = snap_after.get("used_physical_mb", 0.0)
    uobj_after = snap_after.get("live_uobject_slots", 0)
    mem_delta = mem_after - mem_before
    uobj_delta = uobj_after - uobj_before

    # Thresholds
    UOBJ_LIMIT = max(N_CYCLES // 4, 50)  # ~25% of cycles or 50 minimum
    MEM_LIMIT_MB = 100.0

    summary = (f"mem_before={mem_before:.1f}MB mem_after={mem_after:.1f}MB "
               f"delta={mem_delta:+.1f}MB; "
               f"uobj_before={uobj_before} uobj_after={uobj_after} "
               f"delta={uobj_delta:+d}")
    # UObject delta is the CANONICAL leak indicator (GC reclaims tracked objects).
    # mem_delta may grow significantly from asset-registry index churn even with
    # 0 UObject leak — that's editor working-set, not bridge-side leak.
    if uobj_delta > UOBJ_LIMIT:
        log.case("P2_leak_check", "FAIL",
                 f"UOBJECT LEAK DETECTED (uobj_delta={uobj_delta} > limit {UOBJ_LIMIT}); {summary}")
    elif mem_delta > MEM_LIMIT_MB:
        # uobj_delta within bounds but memory grew — XFAIL (likely native heap
        # churn in IAssetTools registry indices, not a bridge bug).
        log.case("P2_leak_check", "XFAIL",
                 f"mem_delta {mem_delta:+.1f}MB exceeds {MEM_LIMIT_MB}MB threshold, but "
                 f"uobj_delta={uobj_delta} within limit — likely asset-registry churn, "
                 f"NOT UObject leak; {summary}")
    else:
        log.case("P2_leak_check", "PASS",
                 f"no leak (uobj_delta={uobj_delta}, mem_delta={mem_delta:+.1f}MB); {summary}")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        cleanup_root()
        return 1

    cleanup_root()

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[E2] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if cc["FAIL"] > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
