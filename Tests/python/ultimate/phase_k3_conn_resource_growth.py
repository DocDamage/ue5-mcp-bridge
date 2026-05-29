#!/usr/bin/env python3
"""Phase K3 — Per-connection resource-growth diagnostic (~6h-death root cause).

The editor died TWICE this session at ~6h uptime / ~500k cumulative MCP
connections, with NO crash dump, the log ending in a flood of
"connection opened -> Recv failed; closing". The bridge spawns one handler
thread per TCP connection — the prime suspect is a slow per-connection
THREAD or OS-HANDLE leak that accumulates over ~500k connections until the
process is OOM/handle-exhausted and the OS terminates it (no UE dump).

E3 churned 5,000 connections and saw no MEMORY leak — but never sampled the
editor's HANDLE / THREAD counts, and 5k << 500k. This phase churns more
connections and directly samples the editor process's HandleCount + Thread
count vs connection count to measure the growth RATE and extrapolate.

Method:
  - resolve UnrealEditor PID; baseline handles + threads
  - churn N connect->1-call->close cycles, sampling handles/threads every
    SAMPLE_EVERY connections (via Get-Process)
  - linear-fit growth per 1000 connections; extrapolate to ~500k

Verdict:
  PASS  — handles & threads flat (slope ~0): per-conn cleanup is correct;
          the ~6h death is NOT a per-connection thread/handle leak (look to
          memory fragmentation / other).
  XFAIL — measurable positive slope: ROOT CAUSE candidate confirmed —
          extrapolated handle/thread count at 500k would approach exhaustion.
          (XFAIL not FAIL: it's the known long-uptime design limit, now
          root-caused, not a new functional break.)

Exit codes: 0=PASS/XFAIL(diagnostic), 1=editor died/transport, 2=preflight.
"""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    Connection,
    TestLogger,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
)

PHASE = "k3"
NAME = "conn_resource_growth"

N_CYCLES = 40000
SAMPLE_EVERY = 4000


def _editor_handles_threads() -> Optional[Tuple[int, int]]:
    """Return (HandleCount, ThreadCount) for UnrealEditor, or None."""
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "$p=Get-Process UnrealEditor -ErrorAction SilentlyContinue;"
             "if($p){\"$($p.HandleCount) $($p.Threads.Count)\"}else{'DEAD'}"],
            capture_output=True, text=True, timeout=15)
        out = (r.stdout or "").strip()
        if out == "DEAD" or not out:
            return None
        parts = out.split()
        return (int(parts[0]), int(parts[1]))
    except Exception:
        return None


def _one_cycle() -> bool:
    try:
        with Connection(connect_timeout=4.0) as conn:
            r = conn.call_keepalive("memreport.get_quick_stats", {}, timeout=6.0)
            return is_ok(r)
    except (socket.timeout, OSError):
        return False
    except Exception:
        return False


def _slope_per_1k(samples: List[Tuple[int, int]]) -> float:
    """Least-squares slope of value vs connection-count, scaled per 1000 conns.
    samples: list of (conn_count, value)."""
    n = len(samples)
    if n < 2:
        return 0.0
    sx = sum(s[0] for s in samples)
    sy = sum(s[1] for s in samples)
    sxx = sum(s[0] * s[0] for s in samples)
    sxy = sum(s[0] * s[1] for s in samples)
    denom = n * sxx - sx * sx
    if denom == 0:
        return 0.0
    slope = (n * sxy - sx * sy) / denom
    return slope * 1000.0


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()

    print(f"[K3] per-connection resource-growth diagnostic ({N_CYCLES} conns)…",
          flush=True)

    base = _editor_handles_threads()
    if base is None:
        log.case("P0_baseline", "FAIL", "couldn't read editor handles/threads")
        log.write()
        return 1
    base_h, base_t = base
    log.case("P0_baseline", "PASS", f"handles={base_h} threads={base_t}")

    h_samples: List[Tuple[int, int]] = [(0, base_h)]
    t_samples: List[Tuple[int, int]] = [(0, base_t)]
    ok_count = 0
    t0 = time.monotonic()
    for i in range(N_CYCLES):
        if _one_cycle():
            ok_count += 1
        if (i + 1) % SAMPLE_EVERY == 0:
            ht = _editor_handles_threads()
            if ht is None:
                log.case("midchurn_health", "FAIL",
                         f"editor DIED at ~{i+1} connections", alive=False)
                log.write()
                return 1
            h_samples.append((i + 1, ht[0]))
            t_samples.append((i + 1, ht[1]))
            print(f"  [K3] conn={i+1} handles={ht[0]} threads={ht[1]} "
                  f"ok={ok_count}/{i+1}", flush=True)
    churn_dt = (time.monotonic() - t0)

    final = _editor_handles_threads() or (base_h, base_t)
    h_slope = _slope_per_1k(h_samples)
    t_slope = _slope_per_1k(t_samples)
    # Extrapolate to 500k connections (the observed death point).
    h_at_500k = base_h + h_slope * 500
    t_at_500k = base_t + t_slope * 500

    log.case("P1_churn", "PASS",
             f"{ok_count}/{N_CYCLES} clean RTs in {churn_dt:.0f}s "
             f"(~{N_CYCLES/churn_dt:.0f}/s)", duration_ms=churn_dt * 1000)

    h_detail = (f"handles {base_h}->{final[0]} slope={h_slope:+.2f}/1k "
                f"extrap@500k={h_at_500k:.0f}")
    t_detail = (f"threads {base_t}->{final[1]} slope={t_slope:+.2f}/1k "
                f"extrap@500k={t_at_500k:.0f}")

    # Thread verdict: a per-connection thread leak is the prime suspect.
    # Healthy: slope ~0 (threads reaped). A slope >= 1/1k means ~1 thread
    # retained per 1000 conns -> +500 threads by 500k (plausible exhaustion).
    if t_slope >= 1.0:
        log.case("P2_thread_growth", "XFAIL",
                 f"ROOT-CAUSE CANDIDATE: thread count grows with connections; "
                 f"{t_detail}")
    else:
        log.case("P2_thread_growth", "PASS",
                 f"threads stable (no per-conn thread leak); {t_detail}")

    # Handle verdict: OS handles (sockets/events). Win handle ceiling is ~16M
    # per process but UE + fragmentation can fail far sooner; a steady positive
    # slope over 500k is the concern.
    if h_slope >= 2.0:
        log.case("P3_handle_growth", "XFAIL",
                 f"ROOT-CAUSE CANDIDATE: handle count grows with connections; "
                 f"{h_detail}")
    else:
        log.case("P3_handle_growth", "PASS",
                 f"handles stable (no per-conn handle leak); {h_detail}")

    if not health(timeout=8.0):
        log.case("post_health", "FAIL", "editor unresponsive after churn", alive=False)
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
    print(f"[K3] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"TOTAL={cc['TOTAL']}")
    print(f"     {h_detail}")
    print(f"     {t_detail}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
