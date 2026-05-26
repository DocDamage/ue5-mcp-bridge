#!/usr/bin/env python3
"""Phase I1 — Historical S+X crash regression matrix.

Goal: re-fire one targeted probe per S+5..S+20 fix to verify each
historical crash class stays closed. Single short script instead of
re-running the heavyweight B1-B9 sweeps.

Per-regression contract:
  * Send the hostile request that originally crashed the editor
  * Expect a structured Bridge error (any code in -32700..-32000)
  * Editor must be alive after each probe
  * No new crash dump

PASS = probe returns structured error OR ok=true (some fixes turn the
crash into a clean success). FAIL = editor death OR transport timeout.

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
    random_suffix,
)

PHASE = "i1"
NAME = "regression"


# (sid, method, args, summary) — one probe per historical fix.
# Each crashed the editor pre-fix; post-fix should return structured error
# (or ok in a few "clean success" cases).
REGRESSIONS: List[Tuple[str, str, Dict[str, Any], str]] = [
    # S+5 PC_Real subcategory validation — crash on bp.add_variable with bad pin subcategory
    ("S+5", "bp.add_variable",
     {"blueprint_path": "/Engine/EngineBlueprints/Untitled",
      "variable_name": "X",
      "pin_type": {"category": "Real", "subcategory": "NotAValidSubcat"}},
     "PC_Real with invalid subcategory"),

    # S+6 FName length on function/variable names — crash on bp.add_function with overflow
    ("S+6", "bp.add_function",
     {"blueprint_path": "/Game/_phantom_bp/X", "function_name": "F" * 1100},
     "FName 1100-char function name"),

    # S+7 IsWriteableMountPoint /Engine block — crash on cb.duplicate to /Engine
    ("S+7", "cb.duplicate",
     {"source_path": "/Engine/BasicShapes/Cube.Cube",
      "dest_path": "/Engine/PhT_I1_Pwn"},
     "cb.duplicate to /Engine"),

    # S+8 cb.create_folder PIE guard — would crash if called during PIE
    # (PIE off in this run → should just create folder; that's PASS too)
    ("S+8", "cb.create_folder",
     {"path": f"/Game/PhT_I1_S8_{random_suffix(4)}"},
     "cb.create_folder PIE guard (no PIE → expect ok)"),

    # S+9 PIE start cooldown — would crash on rapid restart
    # (not exercised here unless PIE was just stopped; just verify pie.start doesn't crash)
    ("S+9", "pie.is_running", {},
     "PIE state query (cooldown is sequential — exercised in C4)"),

    # S+10 centralized FName length — same as S+6 but on a different tool
    ("S+10", "cfg.set_cvar",
     {"name": "X" * 1100, "value": "0"},
     "cfg.set_cvar with 1100-char name"),

    # S+11 mesh.duplicate writeable-mount — crash on dest=/Engine
    ("S+11", "mesh.duplicate",
     {"source_mesh_path": "/Engine/BasicShapes/Cube.Cube",
      "dest_path": "/Engine/PhT_I1_S11_Pwn"},
     "mesh.duplicate to /Engine"),

    # S+12 level.duplicate writeable-mount — crash on dest=/Memory
    ("S+12", "level.duplicate",
     {"source_map": "/Engine/Maps/Templates/OpenWorld",
      "dest_map": "/Memory/PhT_I1_S12_Pwn"},
     "level.duplicate to /Memory"),

    # S+13 cfg.set_cvar FName-internal crash on long
    ("S+13", "cfg.set_cvar",
     {"name": "r.Tonemapper.Sharpen", "value": "X" * 1100},
     "cfg.set_cvar 1100-char value"),

    # S+14 log.set_category_verbosity FName-from-category crash
    ("S+14", "log.set_category_verbosity",
     {"category": "L" * 1100, "verbosity": "Display"},
     "log.set_category_verbosity 1100-char category"),

    # S+15 niagara.set_user_param FName-from-name crash
    ("S+15", "niagara.set_user_param",
     {"actor_path": "/Game/_phantom", "name": "N" * 1100, "value": 1.0},
     "niagara.set_user_param 1100-char name"),

    # S+16 FTopLevelAssetPath pre-validate
    ("S+16", "asset.search_by_class",
     {"class_names": ["X" * 1100]},
     "asset.search_by_class with malformed class name"),

    # S+17 actor.spawn long class_path
    ("S+17", "actor.spawn",
     {"class_path": "/Game/" + "X" * 1100, "location": {"x": 0, "y": 0, "z": 0}},
     "actor.spawn 1100-char class_path"),

    # S+18 // empty segment path crash
    ("S+18a", "asset.get_property",
     {"asset_path": "/Game//_PhT_I1_S18a", "property_path": "Anything"},
     "asset.get_property with //"),
    ("S+18b", "actor.spawn",
     {"class_path": "/Game//_PhT_I1_S18b", "location": {"x": 0, "y": 0, "z": 0}},
     "actor.spawn with // class_path"),

    # S+19 Normalize /./ + URL-encoded
    ("S+19a", "bp.create_blueprint",
     {"dest_path": "/Game/./PhT_I1_S19a",
      "parent_class_path": "/Script/Engine.Actor"},
     "bp.create_blueprint with /./"),
    ("S+19b", "cb.duplicate",
     {"source_path": "/Game/%2E%2E/PhT_I1_S19b",
      "dest_path": "/Game/PhT_I1_S19b_dst"},
     "cb.duplicate with URL-encoded source"),
    ("S+19c", "level.duplicate",
     {"source_map": "/Engine/Maps/Templates/OpenWorld",
      "dest_map": "/Game/PhT_I1\x01/_S19c"},
     "level.duplicate with control char"),

    # S+20 JSON depth cap — exercised via B8 directly; just verify a normal
    # deep-but-bounded payload still works (round-trip sanity)
    ("S+20", "memreport.get_quick_stats",
     {"a": {"a": {"a": {"a": {"a": "leaf"}}}}},  # depth 5 — well under 64
     "depth-5 JSON (under cap, should succeed)"),
]


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[I1] running {len(REGRESSIONS)} historical S+X regression probes…", flush=True)

    for (sid, method, args, summary) in REGRESSIONS:
        label = f"{sid} :: {method} :: {summary[:50]}"
        t0 = time.monotonic()
        try:
            r = call(method, args, timeout=8.0)
        except Exception as e:
            log.case(label, "FAIL", f"exception: {e}",
                     duration_ms=(time.monotonic() - t0) * 1000.0)
            fail_total += 1
            continue
        dt = (time.monotonic() - t0) * 1000.0
        alive = health(timeout=4.0)

        if not alive:
            log.case(label, "FAIL", f"EDITOR DIED on {sid} regression",
                     alive=False, duration_ms=dt)
            log.write()
            return 1
        crash = latest_crash_dump(since=crash_baseline)
        if crash:
            log.case(label, "FAIL", f"CRASH DUMP: {crash}",
                     alive=alive, duration_ms=dt)
            log.write()
            return 1
        if is_transport_failure(r):
            log.case(label, "FAIL", f"transport: {r.get('_err')}",
                     alive=alive, duration_ms=dt)
            fail_total += 1
            continue
        # PASS = either ok=true (some fixes turn the crash into clean success)
        # OR any structured Bridge error in -32700..-32000.
        c = err_code(r)
        if is_ok(r):
            log.case(label, "PASS", f"ok=true (post-fix turned crash into clean success)",
                     alive=alive, duration_ms=dt)
        elif c is not None and -32700 <= c <= -32000:
            log.case(label, "PASS",
                     f"structured error {c}: {err_message(r)[:50]}",
                     alive=alive, duration_ms=dt, code=c)
        else:
            log.case(label, "FAIL",
                     f"unexpected response: code={c}: {err_message(r)[:60]}",
                     alive=alive, duration_ms=dt)
            fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[I1] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
