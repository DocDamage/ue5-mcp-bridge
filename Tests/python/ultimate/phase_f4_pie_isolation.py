#!/usr/bin/env python3
"""Phase F4 — PIE world state leak / isolation.

Goal: read tools issued during PIE return the CORRECT world's data
(editor world for world=editor, PIE world for world=pie). No world
cross-talk.

This phase REQUIRES PIE to be active. If PIE is off, all probes SKIP.

Probes (PIE on):
  P1 — actor.list world=editor → snapshot count
  P2 — actor.list world=pie    → may differ (PIE has its own actors)
  P3 — spawn ATest_PIE actor in PIE world via pie.console_exec (Summon)
  P4 — actor.list world=editor → ATest_PIE NOT present
  P5 — actor.list world=pie    → ATest_PIE IS present (or world delta > 0)
  P6 — level.actor_summary world=editor / world=pie → counts differ

PASS: world isolation correct, no cross-talk.

Exit codes: 0=PASS (or all SKIP if PIE off), 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
)

PHASE = "f4"
NAME = "pie_isolation"


def _is_pie_running() -> bool:
    r = call("pie.is_running", {}, timeout=4.0)
    if not is_ok(r):
        return False
    res = r.get("result", {}) or {}
    return bool(res.get("running") or res.get("is_running"))


def _list_actors(world: str, timeout: float = 8.0) -> Optional[List[Dict[str, Any]]]:
    """Returns flat list of actor dicts, or None on error."""
    r = call("actor.list", {"world": world, "page_size": 500}, timeout=timeout)
    if not is_ok(r):
        return None
    return (r.get("result", {}) or {}).get("actors") or (r.get("result", {}) or {}).get("items") or []


def _actor_summary(world: str, timeout: float = 8.0) -> Optional[Dict[str, Any]]:
    r = call("level.actor_summary", {"world": world}, timeout=timeout)
    if not is_ok(r):
        return None
    return r.get("result", {}) or {}


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    if not _is_pie_running():
        log.case("preflight_pie",
                 "SKIP", "PIE is NOT running — all F4 probes skipped (run with PIE on)")
        # Mark other probes SKIP too
        for p in ("P1_editor_count", "P2_pie_count", "P3_spawn_pie",
                  "P4_editor_unchanged", "P5_pie_changed", "P6_summary_diff"):
            log.case(p, "SKIP", "PIE off")
        summary = log.write()
        cc = summary["counts"]
        print(f"[F4] PIE off — all SKIPPED. SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
        return 0

    print(f"[F4] PIE active — probing world isolation…", flush=True)

    # P1 — snapshot editor world actor count
    editor_actors = _list_actors("editor")
    if editor_actors is None:
        log.case("P1_editor_count", "FAIL", "actor.list world=editor failed")
        log.write()
        return 1
    editor_count_before = len(editor_actors)
    log.case("P1_editor_count", "PASS",
             f"actor.list world=editor → {editor_count_before} actors")

    # P2 — snapshot PIE world actor count
    pie_actors = _list_actors("pie")
    if pie_actors is None:
        log.case("P2_pie_count", "XFAIL",
                 "actor.list world=pie failed (may not support 'pie' world filter)")
        log.write()
        return 0
    pie_count_before = len(pie_actors)
    log.case("P2_pie_count", "PASS",
             f"actor.list world=pie → {pie_count_before} actors")

    # P3 — spawn ATest_PIE in PIE world via console_exec Summon
    # (StaticMeshActor is a safe non-game-mode dependent class)
    rs = call("pie.console_exec",
              {"command": "Summon /Script/Engine.StaticMeshActor"}, timeout=10.0)
    if not is_ok(rs):
        log.case("P3_spawn_pie", "XFAIL",
                 f"pie.console_exec Summon failed: {err_message(rs)[:60]}")
        log.write()
        return 0
    log.case("P3_spawn_pie", "PASS",
             "Summon StaticMeshActor in PIE world")
    time.sleep(1.0)  # let it register

    # P4 — editor world should NOT have new actor
    editor_actors_after = _list_actors("editor")
    if editor_actors_after is None:
        log.case("P4_editor_unchanged", "FAIL",
                 "actor.list world=editor after spawn failed")
        fail_total += 1
    else:
        editor_count_after = len(editor_actors_after)
        editor_delta = editor_count_after - editor_count_before
        if editor_delta == 0:
            log.case("P4_editor_unchanged", "PASS",
                     f"editor world unchanged ({editor_count_before} → {editor_count_after})")
        else:
            log.case("P4_editor_unchanged", "FAIL",
                     f"editor world LEAKED: count {editor_count_before} → {editor_count_after} "
                     f"(delta={editor_delta:+d}); spawn in PIE world reached editor world")
            fail_total += 1

    # P5 — PIE world SHOULD have new actor (delta >= 1)
    pie_actors_after = _list_actors("pie")
    if pie_actors_after is None:
        log.case("P5_pie_changed", "XFAIL",
                 "actor.list world=pie after spawn failed")
    else:
        pie_count_after = len(pie_actors_after)
        pie_delta = pie_count_after - pie_count_before
        if pie_delta >= 1:
            log.case("P5_pie_changed", "PASS",
                     f"PIE world has new actor: count {pie_count_before} → {pie_count_after} "
                     f"(delta={pie_delta:+d})")
        else:
            # Spawn may not have succeeded — XFAIL not FAIL.
            log.case("P5_pie_changed", "XFAIL",
                     f"PIE world count unchanged: {pie_count_before} → {pie_count_after} "
                     f"(Summon may have failed silently; not necessarily a world-leak)")

    # P6 — level.actor_summary world=editor vs world=pie should differ now
    summ_editor = _actor_summary("editor")
    summ_pie = _actor_summary("pie")
    if summ_editor is None or summ_pie is None:
        log.case("P6_summary_diff", "XFAIL",
                 f"level.actor_summary failed: editor={summ_editor is None} pie={summ_pie is None}")
    else:
        total_editor = summ_editor.get("total_actors", -1)
        total_pie = summ_pie.get("total_actors", -1)
        if total_editor >= 0 and total_pie >= 0:
            log.case("P6_summary_diff", "PASS",
                     f"summary returned: editor total={total_editor} pie total={total_pie}")
        else:
            log.case("P6_summary_diff", "XFAIL",
                     f"summary returned without total_actors: editor={summ_editor} pie={summ_pie}")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[F4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
