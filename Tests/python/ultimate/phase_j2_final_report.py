#!/usr/bin/env python3
"""Phase J2 — Final aggregated report generator.

Walks D:/tmp/ws3_stress/test_logs/, aggregates each phase's per-case
results, and emits:
  * D:/tmp/ws3_stress/test_logs/_FINAL_REPORT.md — human-readable
  * D:/tmp/ws3_stress/test_logs/_FINAL_REPORT.json — machine-readable

Report sections:
  1. Run summary (date, total phases, total cases, time-to-here)
  2. Per-category PASS/FAIL/XFAIL/SKIP counts
  3. Per-phase summary table
  4. New crash dumps (scans Saved/Crashes/ for entries since plan creation)
  5. Bridge bug list (S+ numbers reconstructed from commit history)
  6. Known limitations / XFAIL summary

This phase doesn't probe the editor — it's a pure log aggregator.

Exit codes: 0 always (informational).
"""

from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path
from typing import Any, Dict, List

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import LOG_ROOT, PROJECT_CRASH_DIR

PHASE = "j2"
NAME = "final_report"


# Map phase prefix → category
CATEGORY = {
    "a": "A — Functional",
    "b": "B — Crash-safety",
    "c": "C — Concurrency",
    "d": "D — Protocol",
    "e": "E — Stability",
    "f": "F — Security",
    "g": "G — Edge / Recovery",
    "h": "H — Workflows",
    "i": "I — Regression",
    "j": "J — Observability",
}


def _category_for(phase_id: str) -> str:
    return CATEGORY.get(phase_id[:1].lower(), f"?? — Unknown ({phase_id})")


_CASE_LINE_RE = re.compile(
    r"^- \[(PASS|FAIL|XFAIL|SKIP)\] `([^`]+)`.*$",
    re.IGNORECASE
)
_SUMMARY_LINE_RE = re.compile(
    r"PASS=(\d+)\s+FAIL=(\d+)\s+XFAIL=(\d+)\s+SKIP=(\d+)\s+TOTAL=(\d+)",
    re.IGNORECASE
)


def parse_phase_log(md_path: Path) -> Dict[str, Any]:
    """Extract counts + case list from a phase markdown log."""
    counts = {"PASS": 0, "FAIL": 0, "XFAIL": 0, "SKIP": 0, "TOTAL": 0}
    cases: List[Dict[str, str]] = []
    summary_line_found = False
    try:
        text = md_path.read_text(encoding="utf-8", errors="replace")
    except Exception as e:
        return {"counts": counts, "cases": cases, "_err": str(e)}
    for line in text.splitlines():
        m = _SUMMARY_LINE_RE.search(line)
        if m and not summary_line_found:
            counts["PASS"] = int(m.group(1))
            counts["FAIL"] = int(m.group(2))
            counts["XFAIL"] = int(m.group(3))
            counts["SKIP"] = int(m.group(4))
            counts["TOTAL"] = int(m.group(5))
            summary_line_found = True
            continue
        cm = _CASE_LINE_RE.match(line.strip())
        if cm:
            cases.append({"status": cm.group(1).upper(), "case_id": cm.group(2)})
    # Fallback: derive counts from cases if summary line not found.
    if not summary_line_found and cases:
        for c in cases:
            counts[c["status"]] = counts.get(c["status"], 0) + 1
            counts["TOTAL"] += 1
    return {"counts": counts, "cases": cases}


def find_crash_dumps_since(crash_dir: Path, since_ts: float) -> List[Dict[str, Any]]:
    if not crash_dir.exists():
        return []
    out = []
    for entry in crash_dir.iterdir():
        if not entry.is_dir():
            continue
        try:
            mtime = entry.stat().st_mtime
        except OSError:
            continue
        if mtime < since_ts:
            continue
        ctx_xml = entry / "CrashContext.runtime-xml"
        err_msg = ""
        if ctx_xml.exists():
            try:
                t = ctx_xml.read_text(encoding="utf-8", errors="replace")
                m = re.search(r"<ErrorMessage>([^<]+)</ErrorMessage>", t)
                if m:
                    err_msg = m.group(1)
            except Exception:
                pass
        out.append({
            "name": entry.name,
            "mtime": mtime,
            "mtime_human": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(mtime)),
            "error_message": err_msg,
        })
    out.sort(key=lambda d: d["mtime"], reverse=True)
    return out


def main() -> int:
    t_start = time.time()
    log_dir = LOG_ROOT
    if not log_dir.exists():
        print(f"[J2] log directory {log_dir} does not exist — nothing to aggregate.")
        return 0

    # Find phase logs (phase_id_name.md)
    phase_logs = sorted([p for p in log_dir.glob("*.md")
                          if not p.name.startswith("_")])
    print(f"[J2] aggregating {len(phase_logs)} phase logs from {log_dir}…", flush=True)

    # Per-phase counts
    phase_summaries: List[Dict[str, Any]] = []
    cat_totals: Dict[str, Dict[str, int]] = {}
    grand_total = {"PASS": 0, "FAIL": 0, "XFAIL": 0, "SKIP": 0, "TOTAL": 0}
    all_xfails: List[str] = []
    all_fails: List[str] = []

    for md in phase_logs:
        # phase_id = "a1" from "a1_tool_inventory.md"
        phase_id = md.stem.split("_", 1)[0].lower()
        cat = _category_for(phase_id)
        data = parse_phase_log(md)
        cnt = data["counts"]
        phase_summaries.append({
            "phase_id": phase_id,
            "log_name": md.name,
            "category": cat,
            "counts": cnt,
        })
        cat_totals.setdefault(cat, {"PASS": 0, "FAIL": 0, "XFAIL": 0, "SKIP": 0, "TOTAL": 0})
        for k in ("PASS", "FAIL", "XFAIL", "SKIP", "TOTAL"):
            cat_totals[cat][k] += cnt.get(k, 0)
            grand_total[k] += cnt.get(k, 0)
        for c in data["cases"]:
            if c["status"] == "XFAIL":
                all_xfails.append(f"{phase_id}: {c['case_id']}")
            elif c["status"] == "FAIL":
                all_fails.append(f"{phase_id}: {c['case_id']}")

    # Crash dump scan since ULTIMATE plan creation (~Session 1, May 24, 2026 epoch)
    # ~ 2026-05-24 00:00 = use plan file mtime as anchor
    plan_path = Path("D:/tmp/ws3_stress/ULTIMATE_TEST_PLAN.md")
    crash_since = plan_path.stat().st_mtime if plan_path.exists() else (t_start - 7 * 86400)
    crash_dumps = find_crash_dumps_since(PROJECT_CRASH_DIR, crash_since)

    # Bridge fixes (Sx) — hardcoded knowledge from session notes
    BRIDGE_FIXES = [
        ("S+5", "PC_Real subcategory validation"),
        ("S+5b", "PC_Real validation mirror in TerminalTypeFromJson"),
        ("S+6", "FName length crash prevention"),
        ("S+7", "IsWriteableMountPoint blocks /Engine/Script/Memory"),
        ("S+8", "cb.create_folder PIE guard"),
        ("S+9", "PIE start/stop cooldown guard"),
        ("S+10", "centralized FName length validation"),
        ("S+11", "mesh.duplicate writeable-mount guard"),
        ("S+12", "level.duplicate writeable-mount guard"),
        ("S+13", "cfg.get_cvar/set_cvar FName-internal crash"),
        ("S+14", "log.set_category_verbosity FName-from-category crash"),
        ("S+15", "niagara.set_user_param FName-from-name crash guard"),
        ("S+16", "FTopLevelAssetPath pre-validate"),
        ("S+17", "actor.spawn long class_path crash"),
        ("S+18", "// empty segment path crash (3 paths)"),
        ("S+19", "Normalize /./ + URL-encoded + NormaliseMapPath harden"),
        ("S+20", "JSON depth cap in HandleFrame (stack overflow)"),
    ]

    # ─── Write JSON report ──────────────────────────────────────────────────
    report = {
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t_start)),
        "log_dir": str(log_dir),
        "n_phases_aggregated": len(phase_summaries),
        "grand_total": grand_total,
        "category_totals": cat_totals,
        "phases": phase_summaries,
        "bridge_fixes": [{"id": s, "summary": d} for s, d in BRIDGE_FIXES],
        "crash_dumps_since_plan": crash_dumps,
        "all_fails": all_fails,
        "all_xfails": all_xfails,
    }
    json_path = log_dir / "_FINAL_REPORT.json"
    json_path.write_text(json.dumps(report, indent=2, ensure_ascii=False),
                          encoding="utf-8")

    # ─── Write Markdown report ──────────────────────────────────────────────
    md_path = log_dir / "_FINAL_REPORT.md"
    lines: List[str] = []
    lines.append("# ULTIMATE Test Suite — Final Aggregated Report")
    lines.append("")
    lines.append(f"- Generated: {report['generated_at']}")
    lines.append(f"- Log directory: `{log_dir}`")
    lines.append(f"- Phases aggregated: {report['n_phases_aggregated']}")
    lines.append("")
    lines.append("## Grand total")
    lines.append("")
    lines.append("| PASS | FAIL | XFAIL | SKIP | TOTAL |")
    lines.append("|------|------|-------|------|-------|")
    gt = grand_total
    lines.append(f"| **{gt['PASS']}** | **{gt['FAIL']}** | {gt['XFAIL']} | {gt['SKIP']} | {gt['TOTAL']} |")
    lines.append("")
    lines.append("## Per-category totals")
    lines.append("")
    lines.append("| Category | PASS | FAIL | XFAIL | SKIP | TOTAL |")
    lines.append("|---|---|---|---|---|---|")
    for cat, c in sorted(cat_totals.items()):
        lines.append(f"| {cat} | {c['PASS']} | {c['FAIL']} | {c['XFAIL']} | {c['SKIP']} | {c['TOTAL']} |")
    lines.append("")
    lines.append("## Per-phase summary")
    lines.append("")
    lines.append("| Phase | Category | PASS | FAIL | XFAIL | SKIP | TOTAL |")
    lines.append("|---|---|---|---|---|---|---|")
    for p in sorted(phase_summaries, key=lambda x: x["phase_id"]):
        c = p["counts"]
        lines.append(f"| {p['phase_id']} | {p['category']} | {c['PASS']} | {c['FAIL']} | "
                     f"{c['XFAIL']} | {c['SKIP']} | {c['TOTAL']} |")
    lines.append("")
    lines.append("## Bridge bugs found + fixed across this work-package")
    lines.append("")
    for sid, sumr in BRIDGE_FIXES:
        lines.append(f"- **{sid}** — {sumr}")
    lines.append("")
    lines.append(f"## Crash dumps since plan creation ({len(crash_dumps)} found)")
    lines.append("")
    if not crash_dumps:
        lines.append("**No crash dumps since plan creation.**")
    else:
        for cd in crash_dumps:
            lines.append(f"- `{cd['name']}` @ {cd['mtime_human']} — {cd['error_message'] or '(no error message)'}")
    lines.append("")
    if all_fails:
        lines.append(f"## All FAIL cases ({len(all_fails)})")
        lines.append("")
        for f in all_fails:
            lines.append(f"- {f}")
        lines.append("")
    if all_xfails:
        lines.append(f"## All XFAIL cases ({len(all_xfails)} — documented limitations)")
        lines.append("")
        for x in all_xfails[:50]:
            lines.append(f"- {x}")
        if len(all_xfails) > 50:
            lines.append(f"- ... and {len(all_xfails) - 50} more (see _FINAL_REPORT.json)")
        lines.append("")
    lines.append("---")
    lines.append("*Generated by phase_j2_final_report.py*")
    md_path.write_text("\n".join(lines), encoding="utf-8")

    print(f"[J2] wrote {md_path}")
    print(f"[J2] wrote {json_path}")
    print(f"[J2] Grand total: PASS={gt['PASS']} FAIL={gt['FAIL']} "
          f"XFAIL={gt['XFAIL']} SKIP={gt['SKIP']} TOTAL={gt['TOTAL']}")
    print(f"[J2] phases={len(phase_summaries)} categories={len(cat_totals)} "
          f"bridge_fixes={len(BRIDGE_FIXES)} crash_dumps={len(crash_dumps)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
