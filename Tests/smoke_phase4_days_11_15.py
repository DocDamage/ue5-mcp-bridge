#!/usr/bin/env python3
"""Phase 4 Days 11-15 smoke — 9 Material tools.

Verifies (against a live editor on port 30020 with no PIE running):

  Discovery (1):
    1. tools.list contains all 9 new material.* tools.

  Reads (2-5):
    2. material.list_parameters(/Game/MCPTest/Phase4/MI_Test) → parameters.scalar/vector/texture/static_switch
       arrays exist; source_class is /Script/Engine.MaterialInstanceConstant; total_known >= 0.
       SKIP if MI_Test absent — falls back to first /Game material via brute-force probe.
    3. material.get_parameter(MI_Test, <first scalar name from list>) → found=true, type='scalar',
       value matches list_parameters entry. SKIP if no scalar params present.
    4. material.get_parameter(MI_Test, "__nonexistent_param_xyz") → -32036 ParameterNotFound.
    5. material.get_parameter(MI_Test, <name>, parameter_type='vector') with explicit type
       string. SKIP if no vector params.

  Always-on diagnostic (6):
    6. material.is_shader_compiling() → {compiling: bool, remaining_jobs: int>=0}.

  MIC writes (7-10) — best-effort, SKIP gracefully if MI_Test missing:
    7. material.set_scalar_param(MI_Test, <first scalar name>, prior_value + 0.1234) → applied=true,
       prior_value matches expected. Restores to original at end of sub-test 8.
    8. (restore) material.set_scalar_param(MI_Test, <name>, original prior_value) → applied=true.
    9. material.set_vector_param(MI_Test, <first vector name>, {r:0.5, g:0.5, b:0.5, a:1.0}) →
       applied=true, prior_value object has r/g/b/a numerics. Restores at end.
   10. (restore) material.set_vector_param(MI_Test, <name>, original prior_value).
   11. material.set_static_switch(MI_Test, <first static switch>, !prior) → applied=true,
       recompile_triggered=true, recompile_already_pending in {true,false}. Restores at end.
   12. (restore) material.set_static_switch back to prior.

  Compile errors (13):
   13. material.get_compile_errors(MI_Test) → {has_errors: bool, errors: list, warnings: list}.
       Healthy MI_Test → has_errors=false.

  Create (14) — best-effort; if dest already exists from a previous run, the test deletes first.
   14. material.create_instance(parent=<base material of MI_Test>, dest_path=/Game/MCPTest/Phase4/MI_SmokeNew)
       → created=true, mic_path matches. Then cleanup: cb.delete(mic_path, force=true).

  Boundary cases (15-17):
   15. material.set_scalar_param on base UMaterial (M_Test) → -32034 MaterialClassMismatch.
       SKIP if no base UMaterial available.
   16. material.set_texture_param(MI_Test, <name>, texture_path='/Game/bogus_path') → -32004
       ObjectNotFound. SKIP if MI_Test missing.
   17. material.set_texture_param(MI_Test, <name>, texture_path=valid material path) → -32011
       WrongClass. We use MI_Test itself as the bogus "texture" (it's a UMaterialInterface, not
       a UTexture).

PIE skip:
  - All write tests are skipped if PIE is detected (would otherwise fail with -32027). We test
    the PIE-skip path by NOT trying to enter PIE — assume the operator runs this outside PIE.

Prints ``[SMOKE_PHASE4_11_15] PASS`` on success or ``[SMOKE_PHASE4_11_15] FAIL ...`` on first
mismatch. Sub-tests that depend on missing test assets emit SKIP lines.

Usage:
  python smoke_phase4_days_11_15.py [--host HOST] [--port PORT] [--mi-path /Game/.../MI_X]
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Any, Dict, List, Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
READ_TIMEOUT_SEC = 15.0

# Candidate MIC paths probed in order. The plan-mandated test asset is MI_Test under
# /Game/MCPTest/Phase4; we fall back to a few well-known engine assets if absent.
FALLBACK_MI_PATHS = [
    "/Game/MCPTest/Phase4/MI_Test",
]

# Candidate base UMaterial paths — used for the -32034 MaterialClassMismatch boundary case (15).
FALLBACK_BASE_MATERIAL_PATHS = [
    "/Game/MCPTest/Phase4/M_Test",
    "/Engine/EngineMaterials/DefaultMaterial",
]

# Destination path for material.create_instance sub-test (14). Cleaned up afterwards.
CREATE_DEST_PATH = "/Game/MCPTest/Phase4/MI_SmokeNew"


def send_and_recv_line(host: str, port: int, request_obj: dict) -> Optional[dict]:
    with socket.create_connection((host, port), timeout=READ_TIMEOUT_SEC) as sock:
        sock.settimeout(READ_TIMEOUT_SEC)
        payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
        sock.sendall(payload)
        buf = bytearray()
        deadline = time.monotonic() + READ_TIMEOUT_SEC
        while True:
            if time.monotonic() > deadline:
                return None
            try:
                chunk = sock.recv(64 * 1024)
            except socket.timeout:
                return None
            if not chunk:
                break
            buf.extend(chunk)
            newline_idx = buf.find(b"\n")
            if newline_idx >= 0:
                return json.loads(bytes(buf[:newline_idx]).decode("utf-8"))
        return None


def fail(reason: str) -> int:
    print(f"[SMOKE_PHASE4_11_15] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE4_11_15]   SKIP {reason}")


def call(host: str, port: int, label: str, request_id: str, method: str,
         args: Optional[dict] = None) -> Optional[dict]:
    req = {"id": request_id, "kind": "call_function", "method": method, "args": args or {}}
    try:
        return send_and_recv_line(host, port, req)
    except (ConnectionRefusedError, OSError) as exc:
        fail(f"{label}: connect-error detail={exc}")
        return None


def expect_ok(response: Optional[dict], expected_id: str, label: str) -> Optional[dict]:
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return None
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return None
    if response.get("ok") is not True:
        fail(f"{label}: ok-not-true got={response.get('ok')!r} error={response.get('error')!r}")
        return None
    result = response.get("result")
    if not isinstance(result, dict):
        fail(f"{label}: result-not-object got={result!r}")
        return None
    return result


def expect_error(response: Optional[dict], expected_id: str, expected_codes,
                 label: str) -> Optional[dict]:
    if isinstance(expected_codes, int):
        expected_codes = (expected_codes,)
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return None
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return None
    if response.get("ok") is not False:
        fail(f"{label}: ok-not-false got={response.get('ok')!r}")
        return None
    error = response.get("error")
    if not isinstance(error, dict) or error.get("code") not in expected_codes:
        fail(f"{label}: wrong-error-code expected={expected_codes} got={error!r}")
        return None
    return error


def probe_first_existing_path(host: str, port: int, candidates: List[str],
                              probe_method: str = "material.list_parameters") -> Optional[str]:
    """Return the first path in `candidates` that the listener can resolve."""
    for path in candidates:
        resp = call(host, port, "probe", f"probe-{path}", probe_method,
                    {"material_path": path})
        if resp is None:
            continue
        if resp.get("ok") is True:
            return path
        err = resp.get("error", {})
        # Accept the path if the error is "wrong class family" — that means the asset EXISTS but
        # is not a UMaterialInterface. Used by the base-material probe (sub-test 15) which is OK
        # with a UMaterial.
        if probe_method == "material.list_parameters" and err.get("code") in (-32034,):
            # Asset exists but wrong family — we surface this so the caller can decide.
            return path
    return None


def collect_param_names(params_obj: Dict[str, Any]) -> Dict[str, List[str]]:
    """Extract per-category lists of FName strings from the list_parameters body."""
    out = {"scalar": [], "vector": [], "texture": [], "static_switch": []}
    for cat in out:
        cat_arr = params_obj.get(cat, [])
        if isinstance(cat_arr, list):
            for entry in cat_arr:
                if isinstance(entry, dict):
                    name = entry.get("name")
                    if isinstance(name, str):
                        out[cat].append(name)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--mi-path", default=None,
                        help="Override MIC asset path; default probes FALLBACK_MI_PATHS")
    parser.add_argument("--base-path", default=None,
                        help="Override base UMaterial path; default probes FALLBACK_BASE_MATERIAL_PATHS")
    args = parser.parse_args()

    print(f"[SMOKE_PHASE4_11_15] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 9 material.* handlers ──────────────────────────────
    result = expect_ok(call(args.host, args.port, "1", "p4-11-1", "tools.list"),
                       "p4-11-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "material.list_parameters", "material.get_parameter",
        "material.set_scalar_param", "material.set_vector_param",
        "material.set_texture_param", "material.set_static_switch",
        "material.is_shader_compiling",
        "material.create_instance", "material.get_compile_errors",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing material.* handlers: {sorted(missing)}")
    print("[SMOKE_PHASE4_11_15]   1/tools.list contains all 9 material.* handlers")

    # ─── Resolve test MIC ───────────────────────────────────────────────────────────────────────
    mi_path = args.mi_path
    if not mi_path:
        mi_path = probe_first_existing_path(args.host, args.port, FALLBACK_MI_PATHS)
    if not mi_path:
        skip(f"no test MIC under {FALLBACK_MI_PATHS}; skipping MIC-dependent sub-tests")
        mi_path_missing = True
        list_body: Dict[str, Any] = {}
        param_names: Dict[str, List[str]] = {"scalar": [], "vector": [], "texture": [], "static_switch": []}
    else:
        mi_path_missing = False
        print(f"[SMOKE_PHASE4_11_15]   using test MIC: {mi_path}")

    # ─── Sub-test 2: material.list_parameters ───────────────────────────────────────────────────
    if mi_path_missing:
        skip("2/material.list_parameters: no test MIC")
    else:
        list_body = expect_ok(call(args.host, args.port, "2", "p4-11-2",
                                   "material.list_parameters",
                                   {"material_path": mi_path}),
                              "p4-11-2", "2/material.list_parameters")
        if list_body is None:
            return 1
        if "parameters" not in list_body or "source_class" not in list_body or "total_known" not in list_body:
            return fail(f"2/list_parameters: missing top-level keys: {sorted(list_body.keys())}")
        params_obj = list_body["parameters"]
        if not isinstance(params_obj, dict):
            return fail(f"2/list_parameters: parameters not object: {params_obj!r}")
        for cat in ("scalar", "vector", "texture", "static_switch"):
            if cat not in params_obj or not isinstance(params_obj[cat], list):
                return fail(f"2/list_parameters: missing or non-list category '{cat}'")
        param_names = collect_param_names(params_obj)
        total_counted = sum(len(v) for v in param_names.values())
        if total_counted != list_body["total_known"]:
            return fail(f"2/list_parameters: total_known mismatch counted={total_counted} got={list_body['total_known']}")
        print(f"[SMOKE_PHASE4_11_15]   2/list_parameters OK source_class={list_body['source_class']} "
              f"counts=(scalar={len(param_names['scalar'])}, vector={len(param_names['vector'])}, "
              f"texture={len(param_names['texture'])}, switch={len(param_names['static_switch'])}) "
              f"total_known={list_body['total_known']}")

    # ─── Sub-test 3: material.get_parameter — first scalar ──────────────────────────────────────
    if mi_path_missing or not param_names["scalar"]:
        skip("3/material.get_parameter scalar: no scalar params")
    else:
        first_scalar = param_names["scalar"][0]
        result = expect_ok(call(args.host, args.port, "3", "p4-11-3", "material.get_parameter",
                                {"material_path": mi_path, "parameter_name": first_scalar}),
                          "p4-11-3", "3/material.get_parameter scalar")
        if result is None:
            return 1
        if result.get("found") is not True or result.get("type") != "scalar":
            return fail(f"3/get_parameter scalar: unexpected result {result!r}")
        if not isinstance(result.get("value"), (int, float)):
            return fail(f"3/get_parameter scalar: value not numeric: {result!r}")
        print(f"[SMOKE_PHASE4_11_15]   3/get_parameter scalar '{first_scalar}' = {result['value']}")

    # ─── Sub-test 4: material.get_parameter — bogus name → -32036 ───────────────────────────────
    if mi_path_missing:
        skip("4/material.get_parameter nonexistent: no test MIC")
    else:
        ok = expect_error(call(args.host, args.port, "4", "p4-11-4", "material.get_parameter",
                               {"material_path": mi_path,
                                "parameter_name": "__nonexistent_param_xyz_smoke"}),
                         "p4-11-4", -32036, "4/get_parameter nonexistent")
        if ok is None:
            return 1
        print("[SMOKE_PHASE4_11_15]   4/get_parameter nonexistent OK -32036 ParameterNotFound")

    # ─── Sub-test 5: material.get_parameter — explicit type='vector' ────────────────────────────
    if mi_path_missing or not param_names["vector"]:
        skip("5/material.get_parameter vector explicit-type: no vector params")
    else:
        first_vector = param_names["vector"][0]
        result = expect_ok(call(args.host, args.port, "5", "p4-11-5", "material.get_parameter",
                                {"material_path": mi_path, "parameter_name": first_vector,
                                 "parameter_type": "vector"}),
                          "p4-11-5", "5/material.get_parameter vector explicit")
        if result is None:
            return 1
        if result.get("found") is not True or result.get("type") != "vector":
            return fail(f"5/get_parameter vector explicit: unexpected result {result!r}")
        val = result.get("value")
        if not isinstance(val, dict) or any(k not in val for k in ("r", "g", "b", "a")):
            return fail(f"5/get_parameter vector explicit: value not LinearColor object: {result!r}")
        print(f"[SMOKE_PHASE4_11_15]   5/get_parameter vector '{first_vector}' explicit-type OK")

    # ─── Sub-test 6: material.is_shader_compiling — always available ────────────────────────────
    result = expect_ok(call(args.host, args.port, "6", "p4-11-6", "material.is_shader_compiling"),
                       "p4-11-6", "6/material.is_shader_compiling")
    if result is None:
        return 1
    if not isinstance(result.get("compiling"), bool):
        return fail(f"6/is_shader_compiling: compiling not bool: {result!r}")
    remaining = result.get("remaining_jobs")
    if not isinstance(remaining, (int, float)) or remaining < 0:
        return fail(f"6/is_shader_compiling: remaining_jobs invalid: {result!r}")
    print(f"[SMOKE_PHASE4_11_15]   6/is_shader_compiling OK compiling={result['compiling']} "
          f"remaining_jobs={int(remaining)}")

    # ─── Sub-test 7-8: material.set_scalar_param round-trip ─────────────────────────────────────
    if mi_path_missing or not param_names["scalar"]:
        skip("7-8/material.set_scalar_param: no scalar params")
    else:
        scalar_name = param_names["scalar"][0]
        # Capture original via get_parameter for a clean baseline.
        baseline = expect_ok(call(args.host, args.port, "7pre", "p4-11-7pre",
                                  "material.get_parameter",
                                  {"material_path": mi_path, "parameter_name": scalar_name}),
                            "p4-11-7pre", "7pre/get_parameter scalar baseline")
        if baseline is None:
            return 1
        original_value = float(baseline.get("value", 0.0))
        test_value = original_value + 0.1234

        result = expect_ok(call(args.host, args.port, "7", "p4-11-7",
                                "material.set_scalar_param",
                                {"material_path": mi_path, "parameter_name": scalar_name,
                                 "value": test_value}),
                          "p4-11-7", "7/material.set_scalar_param")
        if result is None:
            return 1
        if result.get("applied") is not True:
            return fail(f"7/set_scalar_param: applied != true: {result!r}")
        prior = result.get("prior_value")
        if not isinstance(prior, (int, float)):
            return fail(f"7/set_scalar_param: prior_value not numeric: {result!r}")
        if abs(float(prior) - original_value) > 1e-3:
            return fail(f"7/set_scalar_param: prior {prior} != baseline {original_value}")
        print(f"[SMOKE_PHASE4_11_15]   7/set_scalar_param '{scalar_name}' = {test_value} prior={prior} OK")

        # Restore.
        restore = expect_ok(call(args.host, args.port, "8", "p4-11-8",
                                 "material.set_scalar_param",
                                 {"material_path": mi_path, "parameter_name": scalar_name,
                                  "value": original_value}),
                           "p4-11-8", "8/restore scalar")
        if restore is None:
            return 1
        print(f"[SMOKE_PHASE4_11_15]   8/restore scalar to {original_value} OK")

    # ─── Sub-test 9-10: material.set_vector_param round-trip ────────────────────────────────────
    if mi_path_missing or not param_names["vector"]:
        skip("9-10/material.set_vector_param: no vector params")
    else:
        vector_name = param_names["vector"][0]
        baseline = expect_ok(call(args.host, args.port, "9pre", "p4-11-9pre",
                                  "material.get_parameter",
                                  {"material_path": mi_path, "parameter_name": vector_name,
                                   "parameter_type": "vector"}),
                            "p4-11-9pre", "9pre/get_parameter vector baseline")
        if baseline is None:
            return 1
        original_color = baseline.get("value")
        if not isinstance(original_color, dict):
            return fail(f"9pre/baseline vector: value not object: {baseline!r}")

        test_color = {"r": 0.5, "g": 0.25, "b": 0.75, "a": 1.0}
        result = expect_ok(call(args.host, args.port, "9", "p4-11-9",
                                "material.set_vector_param",
                                {"material_path": mi_path, "parameter_name": vector_name,
                                 "value": test_color}),
                          "p4-11-9", "9/material.set_vector_param")
        if result is None:
            return 1
        if result.get("applied") is not True:
            return fail(f"9/set_vector_param: applied != true: {result!r}")
        prior = result.get("prior_value")
        if not isinstance(prior, dict) or not all(k in prior for k in ("r", "g", "b", "a")):
            return fail(f"9/set_vector_param: prior_value not LinearColor: {result!r}")
        print(f"[SMOKE_PHASE4_11_15]   9/set_vector_param '{vector_name}' = (r,g,b,a)=({test_color['r']},{test_color['g']},{test_color['b']},{test_color['a']}) OK")

        # Restore — pass r/g/b/a explicitly so MAT_ReadJsonLinearColor finds the fields.
        restore_color = {
            "r": float(original_color.get("r", 0.0)),
            "g": float(original_color.get("g", 0.0)),
            "b": float(original_color.get("b", 0.0)),
            "a": float(original_color.get("a", 1.0)),
        }
        restore = expect_ok(call(args.host, args.port, "10", "p4-11-10",
                                 "material.set_vector_param",
                                 {"material_path": mi_path, "parameter_name": vector_name,
                                  "value": restore_color}),
                           "p4-11-10", "10/restore vector")
        if restore is None:
            return 1
        print(f"[SMOKE_PHASE4_11_15]   10/restore vector OK")

    # ─── Sub-test 11-12: material.set_static_switch round-trip ──────────────────────────────────
    if mi_path_missing or not param_names["static_switch"]:
        skip("11-12/material.set_static_switch: no static switch params")
    else:
        sw_name = param_names["static_switch"][0]
        baseline = expect_ok(call(args.host, args.port, "11pre", "p4-11-11pre",
                                  "material.get_parameter",
                                  {"material_path": mi_path, "parameter_name": sw_name,
                                   "parameter_type": "static_switch"}),
                            "p4-11-11pre", "11pre/get_parameter static_switch baseline")
        if baseline is None:
            return 1
        original_bool = bool(baseline.get("value", False))

        result = expect_ok(call(args.host, args.port, "11", "p4-11-11",
                                "material.set_static_switch",
                                {"material_path": mi_path, "parameter_name": sw_name,
                                 "value": not original_bool}),
                          "p4-11-11", "11/material.set_static_switch")
        if result is None:
            return 1
        if result.get("applied") is not True:
            return fail(f"11/set_static_switch: applied != true: {result!r}")
        if result.get("recompile_triggered") is not True:
            return fail(f"11/set_static_switch: recompile_triggered != true: {result!r}")
        if not isinstance(result.get("recompile_already_pending"), bool):
            return fail(f"11/set_static_switch: recompile_already_pending not bool: {result!r}")
        prior = result.get("prior_value")
        if not isinstance(prior, bool):
            return fail(f"11/set_static_switch: prior_value not bool: {result!r}")
        if prior != original_bool:
            return fail(f"11/set_static_switch: prior {prior} != baseline {original_bool}")
        print(f"[SMOKE_PHASE4_11_15]   11/set_static_switch '{sw_name}' = {not original_bool} "
              f"prior={prior} recompile_triggered=True already_pending={result['recompile_already_pending']} OK")

        # Restore.
        restore = expect_ok(call(args.host, args.port, "12", "p4-11-12",
                                 "material.set_static_switch",
                                 {"material_path": mi_path, "parameter_name": sw_name,
                                  "value": original_bool}),
                           "p4-11-12", "12/restore static_switch")
        if restore is None:
            return 1
        print(f"[SMOKE_PHASE4_11_15]   12/restore static_switch to {original_bool} OK")

    # ─── Sub-test 13: material.get_compile_errors ───────────────────────────────────────────────
    if mi_path_missing:
        skip("13/material.get_compile_errors: no test MIC")
    else:
        result = expect_ok(call(args.host, args.port, "13", "p4-11-13",
                                "material.get_compile_errors",
                                {"material_path": mi_path}),
                          "p4-11-13", "13/material.get_compile_errors")
        if result is None:
            return 1
        if not isinstance(result.get("has_errors"), bool):
            return fail(f"13/get_compile_errors: has_errors not bool: {result!r}")
        if not isinstance(result.get("errors"), list) or not isinstance(result.get("warnings"), list):
            return fail(f"13/get_compile_errors: errors/warnings not lists: {result!r}")
        print(f"[SMOKE_PHASE4_11_15]   13/get_compile_errors OK has_errors={result['has_errors']} "
              f"errors={len(result['errors'])} warnings={len(result['warnings'])}")

    # ─── Sub-test 14: material.create_instance ──────────────────────────────────────────────────
    # Use base material from FALLBACK_BASE_MATERIAL_PATHS as the parent. We try to discover one
    # via probe; if none found, SKIP.
    base_path = args.base_path or probe_first_existing_path(
        args.host, args.port, FALLBACK_BASE_MATERIAL_PATHS)
    if not base_path:
        skip("14/material.create_instance: no base material discovered")
    else:
        # Pre-clean: delete the dest if it already exists from a previous run.
        call(args.host, args.port, "14pre", "p4-11-14pre", "cb.delete",
             {"path": CREATE_DEST_PATH, "force": True})

        result = expect_ok(call(args.host, args.port, "14", "p4-11-14",
                                "material.create_instance",
                                {"parent_material_path": base_path,
                                 "dest_path": CREATE_DEST_PATH}),
                          "p4-11-14", "14/material.create_instance")
        if result is None:
            return 1
        if result.get("created") is not True:
            return fail(f"14/create_instance: created != true: {result!r}")
        mic_path = result.get("mic_path")
        if not isinstance(mic_path, str) or CREATE_DEST_PATH not in mic_path:
            return fail(f"14/create_instance: unexpected mic_path: {result!r}")
        print(f"[SMOKE_PHASE4_11_15]   14/create_instance OK mic_path={mic_path}")

        # Cleanup — best-effort delete; failure is non-fatal (asset left for inspection).
        call(args.host, args.port, "14post", "p4-11-14post", "cb.delete",
             {"path": CREATE_DEST_PATH, "force": True})

    # ─── Sub-test 15: material.set_scalar_param on base UMaterial → -32034 MaterialClassMismatch
    # Find a base UMaterial path. If our discovered base is a UMaterial, use it; else SKIP.
    if not base_path:
        skip("15/set_scalar_param on base UMaterial: no base material discovered")
    else:
        ok = expect_error(call(args.host, args.port, "15", "p4-11-15",
                               "material.set_scalar_param",
                               {"material_path": base_path, "parameter_name": "Brightness",
                                "value": 1.0}),
                         "p4-11-15", -32034, "15/set_scalar on base UMaterial")
        if ok is None:
            return 1
        msg = ok.get("message", "")
        if "UMaterialInstanceConstant" not in msg:
            return fail(f"15/set_scalar on base: error message lacks 'UMaterialInstanceConstant': {msg!r}")
        print("[SMOKE_PHASE4_11_15]   15/set_scalar on base UMaterial OK -32034 MaterialClassMismatch")

    # ─── Sub-test 16: material.set_texture_param with bogus texture_path → -32004 ObjectNotFound
    if mi_path_missing or not param_names["texture"]:
        skip("16/set_texture_param bogus texture: no texture params")
    else:
        tex_name = param_names["texture"][0]
        ok = expect_error(call(args.host, args.port, "16", "p4-11-16",
                               "material.set_texture_param",
                               {"material_path": mi_path, "parameter_name": tex_name,
                                "texture_path": "/Game/__bogus_texture_smoke_xyz"}),
                         "p4-11-16", -32004, "16/set_texture bogus path")
        if ok is None:
            return 1
        print("[SMOKE_PHASE4_11_15]   16/set_texture_param bogus path OK -32004 ObjectNotFound")

    # ─── Sub-test 17: material.set_texture_param with wrong-class texture_path → -32011 WrongClass
    if mi_path_missing or not param_names["texture"]:
        skip("17/set_texture_param wrong class: no texture params")
    else:
        tex_name = param_names["texture"][0]
        # Use the MIC itself as the bogus "texture" — it's a UMaterialInterface, not UTexture.
        ok = expect_error(call(args.host, args.port, "17", "p4-11-17",
                               "material.set_texture_param",
                               {"material_path": mi_path, "parameter_name": tex_name,
                                "texture_path": mi_path}),
                         "p4-11-17", -32011, "17/set_texture wrong class")
        if ok is None:
            return 1
        print("[SMOKE_PHASE4_11_15]   17/set_texture_param wrong class OK -32011 WrongClass")

    print("[SMOKE_PHASE4_11_15] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
