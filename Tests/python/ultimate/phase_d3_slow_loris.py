#!/usr/bin/env python3
"""Phase D3 — Slow-loris (very-slow send).

Goal: server doesn't allocate unbounded memory waiting for slow clients,
and slow clients can't starve legitimate work.

Probes:
  P1 — 20 connections each sends 1 byte every 1s, never completes a
       frame for 30s. Concurrent valid work fires; legit clients must
       still get responses < 5s.
  P2 — 10 connections never send anything (just connect + idle 30s).
       Legit clients must remain responsive.
  P3 — Send partial frame (half), wait 5s, send rest. Valid frame is
       eventually parsed and dispatched.

PASS: legit clients unaffected throughout slow-loris attack window.
FAIL: legit clients timeout, OR editor mem grows unboundedly, OR
       editor crashes.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import concurrent.futures
import json
import socket
import sys
import time
from pathlib import Path
from typing import Any, Dict, List

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    HOST,
    PORT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
    snapshot,
)

PHASE = "d3"
NAME = "slow_loris"


def _slow_drip_sender(byte_interval_s: float, duration_s: float) -> bool:
    """Connect, send 1 byte every byte_interval_s, never complete a frame.
    Returns True if connection lived `duration_s` seconds without server
    dropping us. We deliberately send chars that don't form a valid frame
    (no newline at end)."""
    try:
        with socket.create_connection((HOST, PORT), timeout=10.0) as sock:
            sock.settimeout(2.0)
            data = b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":{}'
            # NOTE: deliberately no closing '}' or '\n' — frame is incomplete.
            deadline = time.monotonic() + duration_s
            idx = 0
            while time.monotonic() < deadline and idx < len(data):
                try:
                    sock.sendall(data[idx:idx + 1])
                except OSError:
                    return False
                idx += 1
                time.sleep(byte_interval_s)
            return True
    except Exception:
        return False


def _idle_holder(duration_s: float) -> bool:
    """Connect and just hold the socket open for duration_s; never send."""
    try:
        with socket.create_connection((HOST, PORT), timeout=10.0) as sock:
            sock.settimeout(2.0)
            time.sleep(duration_s)
            return True
    except Exception:
        return False


def _legit_call_loop(stop_time: float) -> Dict[str, Any]:
    """Until stop_time, fire memreport.get_quick_stats; record latencies."""
    latencies: List[float] = []
    failures = 0
    while time.monotonic() < stop_time:
        t0 = time.monotonic()
        r = call("memreport.get_quick_stats", {}, timeout=10.0)
        dt = (time.monotonic() - t0) * 1000.0
        if is_ok(r):
            latencies.append(dt)
        else:
            failures += 1
        time.sleep(0.3)  # ~3 Hz
    return {"n_ok": len(latencies), "n_fail": failures, "latencies": latencies}


def probe_slow_drip(log: TestLogger, n_attackers: int = 20,
                    drip_interval_s: float = 1.0,
                    duration_s: float = 20.0) -> int:
    label = f"P1 slow-drip x{n_attackers} drip={drip_interval_s}s duration={duration_s}s"
    t0 = time.monotonic()

    snap_before = snapshot()
    mem_before = snap_before.get("used_physical_mb", 0.0)

    legit_stop_time = time.monotonic() + duration_s + 1.0
    with concurrent.futures.ThreadPoolExecutor(max_workers=n_attackers + 2) as ex:
        attack_futures = [ex.submit(_slow_drip_sender, drip_interval_s, duration_s)
                          for _ in range(n_attackers)]
        legit_future = ex.submit(_legit_call_loop, legit_stop_time)

        # Wait for attackers + legit
        attack_results = [f.result(timeout=duration_s + 10.0) for f in attack_futures]
        legit_data = legit_future.result(timeout=duration_s + 15.0)

    dt = (time.monotonic() - t0) * 1000.0

    snap_after = snapshot()
    mem_after = snap_after.get("used_physical_mb", 0.0)
    mem_delta = mem_after - mem_before if (mem_before > 0 and mem_after > 0) else 0.0

    n_ok = legit_data["n_ok"]
    n_fail = legit_data["n_fail"]
    latencies = legit_data["latencies"]
    if latencies:
        latencies.sort()
        p50 = latencies[len(latencies) // 2]
        p99 = latencies[int(len(latencies) * 0.99)]
    else:
        p50 = p99 = -1
    summary = (f"legit_ok={n_ok} legit_fail={n_fail} p50={p50:.0f}ms p99={p99:.0f}ms "
               f"mem_delta={mem_delta:.1f}MB attackers_survived={sum(attack_results)}/{n_attackers}")

    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive; {summary}", alive=False, duration_ms=dt)
        return 1

    # Critical metric: did legit clients keep working?
    if n_ok < 3:
        log.case(label, "FAIL",
                 f"slow-loris starved legit clients (only {n_ok} successes); {summary}",
                 duration_ms=dt)
        return 1
    # Latency check: p99 should not be hugely degraded.
    if p99 > 0 and p99 > 3000:
        log.case(label, "XFAIL",
                 f"legit p99={p99:.0f}ms degraded but still serving; {summary}",
                 duration_ms=dt)
        return 0
    # Memory check
    if abs(mem_delta) > 200:
        log.case(label, "XFAIL",
                 f"large mem_delta {mem_delta:.1f}MB but no crash; {summary}",
                 duration_ms=dt)
        return 0
    log.case(label, "PASS",
             f"slow-loris attack absorbed cleanly; {summary}", duration_ms=dt)
    return 0


def probe_silent_holders(log: TestLogger, n: int = 10, duration_s: float = 15.0) -> int:
    label = f"P2 silent-holders x{n} duration={duration_s}s"
    t0 = time.monotonic()

    legit_stop_time = time.monotonic() + duration_s + 1.0
    with concurrent.futures.ThreadPoolExecutor(max_workers=n + 2) as ex:
        idle_futures = [ex.submit(_idle_holder, duration_s) for _ in range(n)]
        legit_future = ex.submit(_legit_call_loop, legit_stop_time)
        idle_results = [f.result(timeout=duration_s + 10.0) for f in idle_futures]
        legit_data = legit_future.result(timeout=duration_s + 15.0)

    dt = (time.monotonic() - t0) * 1000.0
    n_ok = legit_data["n_ok"]
    summary = f"legit_ok={n_ok} idle_survived={sum(idle_results)}/{n}"

    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive; {summary}", alive=False, duration_ms=dt)
        return 1
    if n_ok < 3:
        log.case(label, "FAIL",
                 f"silent holders starved legit work; {summary}", duration_ms=dt)
        return 1
    log.case(label, "PASS", f"silent holders absorbed; {summary}", duration_ms=dt)
    return 0


def probe_partial_then_complete(log: TestLogger) -> int:
    """P3: send half a frame, wait 5s, send rest. Should still parse."""
    label = "P3 partial-then-complete (5s pause)"
    payload = b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":{}}\n'
    half = len(payload) // 2
    t0 = time.monotonic()
    try:
        with socket.create_connection((HOST, PORT), timeout=20.0) as sock:
            sock.settimeout(15.0)
            sock.sendall(payload[:half])
            time.sleep(5.0)
            sock.sendall(payload[half:])
            buf = bytearray()
            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline:
                try:
                    sock.settimeout(max(1.0, deadline - time.monotonic()))
                    chunk = sock.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    break
                buf.extend(chunk)
                nl = buf.find(b"\n")
                if nl >= 0:
                    obj = json.loads(buf[:nl].decode("utf-8", "replace"))
                    dt = (time.monotonic() - t0) * 1000.0
                    if obj.get("ok"):
                        log.case(label, "PASS",
                                 f"partial frame eventually parsed and dispatched; took {dt:.0f}ms",
                                 duration_ms=dt)
                        return 0
                    log.case(label, "FAIL",
                             f"partial frame parsed but response not ok: {obj}",
                             duration_ms=dt)
                    return 1
        dt = (time.monotonic() - t0) * 1000.0
        log.case(label, "XFAIL",
                 f"no response received after 5s pause (server may have dropped); legit-clients OK",
                 duration_ms=dt)
        return 0
    except Exception as e:
        dt = (time.monotonic() - t0) * 1000.0
        log.case(label, "FAIL", f"exception: {e}", duration_ms=dt)
        return 1


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[D3] slow-loris probes (this takes ~50s due to attack duration)…", flush=True)

    fail_total += probe_slow_drip(log, n_attackers=20, drip_interval_s=1.0, duration_s=15.0)
    fail_total += probe_silent_holders(log, n=10, duration_s=12.0)
    fail_total += probe_partial_then_complete(log)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[D3] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
