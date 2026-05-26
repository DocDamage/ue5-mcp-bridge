#!/usr/bin/env python3
"""Phase B7 — empty/null collection inputs.

Goal: every tool that accepts an array, object, or nullable scalar must
behave deterministically when handed an empty/null container. Either
PASS-with-success (documented no-op semantics) or PASS-with-error
(-32602 / -32004 / -32601 / structured Bridge error). FAIL only if:
  - Editor crashes / dies (most critical — would have to be very bad)
  - Transport timeout
  - Unstructured exception trace in response

Probes deliberately don't satisfy required-field validators by design —
the *positional* arg is empty, so most tools will fail validation early.
What we're guarding against is parsers/handlers that accept an empty
collection and then deref a null pointer or iterate an empty array
into an integer-underflow loop.

Pattern variants per (method, field, kind):
  arr_empty   field=[]
  arr_null    field=null
  obj_empty   field={}
  obj_null    field=null
  str_empty   field=""
  str_null    field=null

Each variant is sent as the sole field in args (other required fields
omitted), so the tool's response can be parsed for crash-safety only.

Exit codes: 0=PASS, 1=FAIL (editor crashed OR transport died), 2=preflight.
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

PHASE = "b7"
NAME = "empty_collections"

# (method, field, kind)  — kind = "array"/"object"/"string"
# Sample drawn from the most-collection-heavy tool signatures across the surface.
# Not exhaustive — purpose is to hit ~13 representative handlers each with
# multiple variants, not to enumerate every collection-taking field.
PROBES: List[Tuple[str, str, str]] = [
    # Asset registry — filter is object, package_paths/class_names are arrays.
    ("asset.list", "filter", "object"),
    ("asset.list", "package_paths", "array"),
    ("asset.search_by_class", "class_names", "array"),
    # BP authoring — parameter lists, pin lists, default-values.
    ("bp.add_function_parameter", "parameters", "array"),
    ("bp.list_function_parameters", "function_name", "string"),
    ("bp.set_pin_default", "value", "object"),
    # AI graphs — keys, nodes.
    ("ai.bb.set_value", "value", "object"),
    ("ai.bb.set_value", "actor_path", "string"),
    ("ai.bt.add_node", "decorators", "array"),
    # Actor / level batch — locations array, actor_paths array.
    ("transform.batch_set", "actor_paths", "array"),
    ("transform.batch_set", "locations", "array"),
    ("actor.list", "class_filter", "string"),
    # Spatial queries — vectors.
    ("actor.box_query", "min", "array"),
    ("actor.box_query", "max", "array"),
    ("actor.sphere_query", "center", "array"),
    # Niagara / material — parameters arrays, expression maps.
    ("niagara.set_user_param", "value", "object"),
    ("mat.set_expression_parameter", "parameters", "object"),
    # Data table / curve — bulk row inputs.
    ("data_table.set_row", "data", "object"),
    ("curve.set_row_value", "value", "object"),
    # Input authoring — mappings array.
    ("input.add_mapping_to_context", "modifiers", "array"),
    # GameplayTag — tag containers.
    ("gameplaytag.add_to_container", "tags", "array"),
    ("gameplaytag.query_actor", "tags", "array"),
    # Config / log — value field with conflicting type.
    ("cfg.set_cvar", "value", "object"),
    ("log.set_category_verbosity", "category", "string"),
    # Sequencer — bindings/sections.
    ("sequencer.add_section_to_track", "section_data", "object"),
    # Physics — impulse vector / force vector.
    ("physics.apply_impulse", "impulse", "array"),
    ("physics.apply_impulse", "actor_path", "string"),
    # Folder ops.
    ("folder.create", "folder_path", "string"),
    ("cb.create_folder", "path", "string"),
    # Hierarchy.
    ("hierarchy.set_parent", "parent_path", "string"),
]


def _payload_for_variant(field: str, variant: str) -> Any:
    """Build the args object for a (field, variant) probe."""
    if variant == "arr_empty":
        return {field: []}
    if variant == "arr_null":
        return {field: None}
    if variant == "obj_empty":
        return {field: {}}
    if variant == "obj_null":
        return {field: None}
    if variant == "str_empty":
        return {field: ""}
    if variant == "str_null":
        return {field: None}
    raise ValueError(variant)


def _variants_for_kind(kind: str) -> List[str]:
    if kind == "array":
        return ["arr_empty", "arr_null"]
    if kind == "object":
        return ["obj_empty", "obj_null"]
    if kind == "string":
        return ["str_empty", "str_null"]
    raise ValueError(kind)


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    total_probes = sum(len(_variants_for_kind(k)) for (_, _, k) in PROBES)
    print(f"[B7] running {total_probes} empty-collection probes "
          f"across {len(PROBES)} (method, field) pairs…", flush=True)

    for (method, field, kind) in PROBES:
        for variant in _variants_for_kind(kind):
            label = f"{method} :: {field}={variant}"
            args = _payload_for_variant(field, variant)
            t0 = time.monotonic()
            try:
                r = call(method, args, timeout=6.0)
            except Exception as e:
                r = {"_err": "exception", "_exc": str(e)}
            dt = (time.monotonic() - t0) * 1000.0
            c = err_code(r)
            alive = health(timeout=3.0)

            if not alive:
                log.case(label, "FAIL",
                         f"EDITOR DIED on {method} {variant}",
                         alive=False, duration_ms=dt)
                log.write()
                print(f"  [B7] EDITOR CRASHED on {method} :: {variant}",
                      file=sys.stderr)
                return 1
            crash = latest_crash_dump(since=crash_baseline)
            if crash:
                log.case(label, "FAIL", f"CRASH DUMP: {crash}",
                         alive=alive, duration_ms=dt, code=c)
                log.write()
                return 1
            if is_transport_failure(r):
                log.case(label, "FAIL", f"transport: {r.get('_err')}",
                         alive=alive, duration_ms=dt)
                fail_total += 1
                continue
            # Any structured Bridge error is PASS — handler refused safely.
            # Success is also PASS — documented no-op semantics with empty
            # container (e.g. tags=[] on remove_from_container is no-op).
            if is_ok(r):
                log.case(label, "PASS",
                         f"handler accepted empty {kind} (no-op semantics)",
                         alive=alive, duration_ms=dt)
            elif c is not None and -32700 <= c <= -32000:
                log.case(label, "PASS",
                         f"guard fired: {c}: {err_message(r)[:60]}",
                         alive=alive, duration_ms=dt, code=c)
            else:
                log.case(label, "FAIL",
                         f"unexpected response: code={c}: {err_message(r)[:60]}",
                         alive=alive, duration_ms=dt, code=c)
                fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[B7] PASS={cc['PASS']} FAIL={cc['FAIL']} "
          f"XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
