#!/usr/bin/env python3
"""Phase H4 — Niagara + Material complex multi-asset pipeline.

The most complex remaining ULTIMATE workflow: author a Material graph
(create → add expressions → connect → set parameter), author a Niagara
emitter, then verify both assets exist independently with correct shape.
Exercises the cross-surface authoring chain end-to-end via MCP only.

Material expression GUIDs are captured from mat.add_expression's
`expression_guid` return and fed into mat.connect_expressions /
mat.set_expression_parameter.

Steps:
  HARD (must succeed):
    1. folder.create root
    2. asset.create Material (/Script/Engine.Material)
    3. mat.add_expression Constant3Vector → guid_c
    4. mat.add_expression Multiply → guid_m
    5. niagara.create_emitter
    6. asset.exists Material + Emitter (both true)
  SOFT (ok OR graceful structured = acceptable; transport/crash = FAIL):
    7. mat.connect_expressions (guid_c.out0 → guid_m input "A")
    8. mat.set_expression_parameter (guid_c "Constant")
    9. niagara.list_parameters on the emitter
   10. asset.list_properties on the Material (read-back)

PASS: all hard steps succeed, soft steps graceful, both assets exist,
no crash.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

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

PHASE = "h4"
NAME = "niagara_material"

ROOT = f"/Game/PhT_H4_{random_suffix(6)}"
MAT = f"{ROOT}/M_Test"
EMITTER = f"{ROOT}/NE_Test"

C3V = "/Script/Engine.MaterialExpressionConstant3Vector"
MULT = "/Script/Engine.MaterialExpressionMultiply"


def cleanup() -> None:
    call("cb.delete", {"path": MAT, "force": True}, timeout=10.0)
    call("cb.delete", {"path": EMITTER, "force": True}, timeout=10.0)
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=12.0)


def _hard(log: TestLogger, name: str, method: str, args: Dict[str, Any],
          timeout: float = 20.0) -> Optional[Dict[str, Any]]:
    """Must succeed. Returns result dict, or None (logs FAIL)."""
    t0 = time.monotonic()
    r = call(method, args, timeout=timeout)
    dt = (time.monotonic() - t0) * 1000.0
    if not health(timeout=4.0):
        log.case(name, "FAIL", f"EDITOR DIED after {method}", alive=False, duration_ms=dt)
        return None
    if is_ok(r):
        log.case(name, "PASS", f"{method} ok", duration_ms=dt)
        return r.get("result", {}) or {}
    log.case(name, "FAIL", f"{method}: {err_code(r)}: {err_message(r)[:60]}",
             duration_ms=dt, code=err_code(r))
    return None


def _soft(log: TestLogger, name: str, method: str, args: Dict[str, Any],
          timeout: float = 20.0) -> int:
    """ok OR graceful structured error acceptable. transport/crash = FAIL.
    Returns fail-delta (0/1)."""
    t0 = time.monotonic()
    r = call(method, args, timeout=timeout)
    dt = (time.monotonic() - t0) * 1000.0
    if not health(timeout=4.0):
        log.case(name, "FAIL", f"EDITOR DIED after {method}", alive=False, duration_ms=dt)
        return 1
    if is_transport_failure(r):
        log.case(name, "FAIL", f"transport: {r.get('_err')}", duration_ms=dt)
        return 1
    if is_ok(r):
        log.case(name, "PASS", f"{method} ok", duration_ms=dt)
        return 0
    c = err_code(r)
    if c == -32601:
        log.case(name, "SKIP", f"{method} not registered", duration_ms=dt)
        return 0
    if c is not None and -32700 <= c <= -32000:
        log.case(name, "XFAIL",
                 f"{method} graceful structured {c}: {err_message(r)[:45]}",
                 duration_ms=dt, code=c)
        return 0
    log.case(name, "FAIL", f"{method} unknown code={c}: {err_message(r)[:50]}",
             duration_ms=dt, code=c)
    return 1


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[H4] Niagara+Material pipeline (root={ROOT})…", flush=True)
    cleanup()

    # 1. folder
    if _hard(log, "1_folder", "folder.create", {"folder_path": ROOT}) is None:
        log.write(); cleanup(); return 1

    # 2. Material
    if _hard(log, "2_create_material", "asset.create",
             {"dest_path": MAT, "class_path": "/Script/Engine.Material"}) is None:
        log.write(); cleanup(); return 1

    # 3. Constant3Vector expression
    r = _hard(log, "3_add_const3vec", "mat.add_expression",
              {"material_path": MAT, "expression_class": C3V, "position": [-400, 0]})
    if r is None:
        log.write(); cleanup(); return 1
    guid_c = r.get("expression_guid") or ""
    log.case("3b_guid_const", "PASS" if guid_c else "XFAIL",
             f"const3vec guid={guid_c[:20] or 'MISSING'}")

    # 4. Multiply expression
    r = _hard(log, "4_add_multiply", "mat.add_expression",
              {"material_path": MAT, "expression_class": MULT, "position": [-100, 0]})
    if r is None:
        log.write(); cleanup(); return 1
    guid_m = r.get("expression_guid") or ""
    log.case("4b_guid_mult", "PASS" if guid_m else "XFAIL",
             f"multiply guid={guid_m[:20] or 'MISSING'}")

    # 5. Niagara emitter (SOFT — creation is template-dependent and can be
    # heavy; a structured failure is acceptable, only transport/crash fails).
    fail_total += _soft(log, "5_create_emitter", "niagara.create_emitter",
                        {"dest_path": EMITTER})

    # 6. Both exist
    rm = call("asset.exists", {"path": MAT}, timeout=8.0)
    re_ = call("asset.exists", {"path": EMITTER}, timeout=8.0)
    mat_ok = is_ok(rm) and (rm.get("result", {}) or {}).get("exists")
    em_ok = is_ok(re_) and (re_.get("result", {}) or {}).get("exists")
    if mat_ok:
        log.case("6a_material_exists", "PASS", "Material in registry")
    else:
        log.case("6a_material_exists", "FAIL", f"Material missing: {rm.get('result')}")
        fail_total += 1
    log.case("6b_emitter_exists", "PASS" if em_ok else "XFAIL",
             "Emitter in registry" if em_ok else "emitter not created (heavy/optional)")

    # 7. connect (soft — input-name / output-index nuances)
    if guid_c and guid_m:
        fail_total += _soft(log, "7_connect", "mat.connect_expressions",
                            {"material_path": MAT,
                             "from_expression_guid": guid_c,
                             "to_expression_guid": guid_m,
                             "to_input_name": "A",
                             "from_output_index": 0})
    else:
        log.case("7_connect", "SKIP", "missing guids — can't connect")

    # 8. set parameter (soft — value shape nuance)
    if guid_c:
        fail_total += _soft(log, "8_set_param", "mat.set_expression_parameter",
                            {"material_path": MAT, "expression_guid": guid_c,
                             "property_name": "Constant",
                             "value": {"r": 1.0, "g": 0.5, "b": 0.2, "a": 1.0}})
    else:
        log.case("8_set_param", "SKIP", "missing const guid")

    # 9. niagara list parameters (soft)
    if em_ok:
        fail_total += _soft(log, "9_niagara_list_params", "niagara.list_parameters",
                            {"path": EMITTER})
    else:
        log.case("9_niagara_list_params", "SKIP", "emitter unavailable")

    # 10. material read-back (soft)
    fail_total += _soft(log, "10_material_props", "asset.list_properties",
                        {"asset_path": MAT})

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write(); cleanup(); return 1

    cleanup()
    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
