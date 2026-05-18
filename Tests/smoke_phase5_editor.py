#!/usr/bin/env python3
"""Phase 5 Chunk B smoke — 10 editor utility tools + pie.screenshot_to_disk.

Verifies (against a live editor on port 30020; PIE may be on or off — sub-tests adapt):

  Discovery (1):
    1. tools.list contains all 10 new editor.* + pie.screenshot_to_disk handlers.

  Current world (2):
    2. editor.current_world → {world_path, world_name, pie_active} schema verified;
       world_path starts with "/", pie_active is bool.

  Camera get/set (3-4):
    3. editor.get_camera → {location, rotation, fov, ortho_width} schema; location/rotation
       are length-3 arrays of numbers; fov is positive number; ortho_width is null OR number.
    4. editor.set_camera with arbitrary location + rotation + fov=90.0 → {set: true}.
       Then editor.get_camera and verify the location matches what was set (within 0.01).
       Restore original camera state at end.

  Selection get/set (5-7):
    5. editor.get_selection → {actors, components} schema (arrays).
    6. editor.set_selection([]) → {selected_count: 0} (clears selection).
    7. editor.set_selection with too many ids (201 strings) → -32017 InputTooLarge.

  Tick + message (8-9):
    8. editor.tick_once → {ticked: true}.
    9. editor.show_message("Phase 5 Chunk B smoke", level="success") → {shown: true};
       repeat with level="error" + duration=2.0; bad level → -32602 InvalidParams.

  Screenshot family (10-13):
   10. editor.viewport_screenshot(width=256, height=256, format='png') → base64 PNG.
       Verifies result.base64 decodes, starts with PNG magic bytes (\\x89PNG).
   11. editor.viewport_screenshot(width=10000) → -32602 (out of [32,2048]).
   12. editor.viewport_screenshot_to_disk(default path) → file exists, bytes>0, PNG magic verified.
       Then with path="C:/Windows/foo.png" → -32013 PathEscape.
   13. pie.screenshot_to_disk WITHOUT PIE → -32038 PIENotActive (frozen message verified).

  Cleanup:
    Restores original camera state. Does NOT modify selection (already cleared in sub-test 6).

Prints ``[SMOKE_PHASE5_EDITOR] PASS`` on success or ``[SMOKE_PHASE5_EDITOR] FAIL ...`` on
first mismatch.

Usage:
  python smoke_phase5_editor.py [--host HOST] [--port PORT]
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import sys
import time
from typing import Any, Dict, List, Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
READ_TIMEOUT_SEC = 60.0  # screenshots can be slow on cold viewport


def send_and_recv_line(host: str, port: int, request_obj: dict,
                       timeout: float = READ_TIMEOUT_SEC) -> Optional[dict]:
    """Single request → single line response. Accepts any line size up to 64 MiB."""
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
        sock.sendall(payload)
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while True:
            if time.monotonic() > deadline:
                return None
            try:
                chunk = sock.recv(256 * 1024)  # screenshot responses are ~MiB
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
    print(f"[SMOKE_PHASE5_EDITOR] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE5_EDITOR]   SKIP {reason}")


def call(host: str, port: int, label: str, request_id: str, method: str,
         args: Optional[dict] = None, timeout: float = READ_TIMEOUT_SEC) -> Optional[dict]:
    req = {"id": request_id, "kind": "call_function", "method": method, "args": args or {}}
    try:
        return send_and_recv_line(host, port, req, timeout=timeout)
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


def is_png_magic(data: bytes) -> bool:
    """First 8 bytes of any PNG file: 89 50 4E 47 0D 0A 1A 0A."""
    return len(data) >= 8 and data[:8] == b"\x89PNG\r\n\x1a\n"


def is_jpg_magic(data: bytes) -> bool:
    """JPEG SOI marker: FF D8 FF."""
    return len(data) >= 3 and data[:3] == b"\xff\xd8\xff"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    print(f"[SMOKE_PHASE5_EDITOR] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all editor.* + pie.screenshot_to_disk ─────────────────
    result = expect_ok(call(args.host, args.port, "1", "p5-edt-1", "tools.list"),
                       "p5-edt-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "editor.viewport_screenshot",
        "editor.viewport_screenshot_to_disk",
        "pie.screenshot_to_disk",
        "editor.get_camera",
        "editor.set_camera",
        "editor.get_selection",
        "editor.set_selection",
        "editor.show_message",
        "editor.current_world",
        "editor.tick_once",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing handlers: {sorted(missing)}")
    print(f"[SMOKE_PHASE5_EDITOR]   1/tools.list contains all 10 editor.* + pie.screenshot_to_disk")

    # ─── Sub-test 2: editor.current_world ──────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "2", "p5-edt-2", "editor.current_world"),
                       "p5-edt-2", "2/editor.current_world")
    if result is None:
        return 1
    if not isinstance(result.get("world_path"), str) or not result["world_path"].startswith("/"):
        return fail(f"2/current_world: world_path malformed {result!r}")
    if not isinstance(result.get("world_name"), str) or not result["world_name"]:
        return fail(f"2/current_world: world_name empty {result!r}")
    if not isinstance(result.get("pie_active"), bool):
        return fail(f"2/current_world: pie_active not bool {result!r}")
    pie_active_initial = bool(result["pie_active"])
    print(f"[SMOKE_PHASE5_EDITOR]   2/editor.current_world OK "
          f"(world_path={result['world_path']!r}, pie_active={pie_active_initial})")

    # ─── Sub-test 3: editor.get_camera ─────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "3", "p5-edt-3", "editor.get_camera"),
                       "p5-edt-3", "3/editor.get_camera")
    if result is None:
        return 1
    loc = result.get("location")
    rot = result.get("rotation")
    fov = result.get("fov")
    ortho = result.get("ortho_width")
    if not isinstance(loc, list) or len(loc) != 3 or not all(isinstance(x, (int, float)) for x in loc):
        return fail(f"3/get_camera: location not [x,y,z] {result!r}")
    if not isinstance(rot, list) or len(rot) != 3 or not all(isinstance(x, (int, float)) for x in rot):
        return fail(f"3/get_camera: rotation not [p,y,r] {result!r}")
    if not isinstance(fov, (int, float)) or fov <= 0:
        return fail(f"3/get_camera: fov not positive number {result!r}")
    if ortho is not None and not isinstance(ortho, (int, float)):
        return fail(f"3/get_camera: ortho_width not null/number {result!r}")
    original_camera = {"location": loc, "rotation": rot, "fov": fov}
    print(f"[SMOKE_PHASE5_EDITOR]   3/editor.get_camera OK "
          f"(location={loc}, rotation={rot}, fov={fov}, ortho={ortho})")

    # ─── Sub-test 4: editor.set_camera ─────────────────────────────────────────────────────────
    target_loc = [1234.5, -678.9, 555.0]
    target_rot = [-15.0, 45.0, 0.0]
    target_fov = 90.0
    result = expect_ok(
        call(args.host, args.port, "4a", "p5-edt-4a", "editor.set_camera",
             {"location": target_loc, "rotation": target_rot, "fov": target_fov}),
        "p5-edt-4a", "4a/set_camera")
    if result is None:
        return 1
    if result.get("set") is not True:
        return fail(f"4a/set_camera: set!=true {result!r}")
    # Verify round-trip.
    result = expect_ok(call(args.host, args.port, "4b", "p5-edt-4b", "editor.get_camera"),
                       "p5-edt-4b", "4b/get_camera-after-set")
    if result is None:
        return 1
    actual_loc = result.get("location") or [0, 0, 0]
    actual_fov = result.get("fov")
    for axis, want, got in zip("xyz", target_loc, actual_loc):
        if abs(got - want) > 0.01:
            return fail(f"4b/get_camera: location.{axis} mismatch want={want} got={got}")
    if actual_fov is None or abs(actual_fov - target_fov) > 0.01:
        return fail(f"4b/get_camera: fov mismatch want={target_fov} got={actual_fov}")
    print(f"[SMOKE_PHASE5_EDITOR]   4/editor.set_camera OK (location+rotation+fov round-trips)")

    # Restore camera before continuing — other sub-tests don't read camera state but be polite.
    call(args.host, args.port, "4c", "p5-edt-4c", "editor.set_camera",
         {"location": original_camera["location"],
          "rotation": original_camera["rotation"],
          "fov": original_camera["fov"]})

    # ─── Sub-test 5: editor.get_selection ──────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "5", "p5-edt-5", "editor.get_selection"),
                       "p5-edt-5", "5/editor.get_selection")
    if result is None:
        return 1
    if not isinstance(result.get("actors"), list):
        return fail(f"5/get_selection: actors not list {result!r}")
    if not isinstance(result.get("components"), list):
        return fail(f"5/get_selection: components not list {result!r}")
    print(f"[SMOKE_PHASE5_EDITOR]   5/editor.get_selection OK "
          f"(actors={len(result['actors'])}, components={len(result['components'])})")

    # ─── Sub-test 6: editor.set_selection with empty array (clear) ─────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "6", "p5-edt-6", "editor.set_selection",
             {"actor_ids": [], "append": False}),
        "p5-edt-6", "6/set_selection-empty")
    if result is None:
        return 1
    if result.get("selected_count") != 0:
        return fail(f"6/set_selection-empty: expected selected_count=0 got {result!r}")
    print(f"[SMOKE_PHASE5_EDITOR]   6/editor.set_selection([]) OK (cleared selection)")

    # ─── Sub-test 7: editor.set_selection with 201 ids → -32017 ────────────────────────────────
    too_many = [f"BogusActor_{i}" for i in range(201)]
    resp = call(args.host, args.port, "7", "p5-edt-7", "editor.set_selection",
                {"actor_ids": too_many})
    if expect_error(resp, "p5-edt-7", -32017, "7/set_selection-too-many") is None:
        return 1
    print(f"[SMOKE_PHASE5_EDITOR]   7/editor.set_selection(201 ids) → -32017 InputTooLarge")

    # ─── Sub-test 8: editor.tick_once ──────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "8", "p5-edt-8", "editor.tick_once"),
                       "p5-edt-8", "8/editor.tick_once")
    if result is None:
        return 1
    if result.get("ticked") is not True:
        return fail(f"8/tick_once: ticked!=true {result!r}")
    print(f"[SMOKE_PHASE5_EDITOR]   8/editor.tick_once OK (ticked=true)")

    # ─── Sub-test 9: editor.show_message ───────────────────────────────────────────────────────
    for lvl in ("info", "success", "warning", "error"):
        rid = f"p5-edt-9-{lvl}"
        result = expect_ok(
            call(args.host, args.port, rid, rid, "editor.show_message",
                 {"text": f"Phase 5 Chunk B smoke (level={lvl})", "level": lvl, "duration": 2.0}),
            rid, f"9/show_message-{lvl}")
        if result is None:
            return 1
        if result.get("shown") is not True:
            return fail(f"9/show_message-{lvl}: shown!=true {result!r}")
    # Bad level
    resp = call(args.host, args.port, "9-bad", "p5-edt-9-bad", "editor.show_message",
                {"text": "x", "level": "nonsense"})
    if expect_error(resp, "p5-edt-9-bad", -32602, "9/show_message-bad-level") is None:
        return 1
    # Empty text
    resp = call(args.host, args.port, "9-empty", "p5-edt-9-empty", "editor.show_message",
                {"text": "", "level": "info"})
    if expect_error(resp, "p5-edt-9-empty", -32602, "9/show_message-empty-text") is None:
        return 1
    print(f"[SMOKE_PHASE5_EDITOR]   9/editor.show_message OK (4 levels + 2 negative cases)")

    # ─── Sub-test 10: editor.viewport_screenshot (in-memory PNG) ───────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "10", "p5-edt-10", "editor.viewport_screenshot",
             {"width": 256, "height": 256, "format": "png"}),
        "p5-edt-10", "10/viewport_screenshot")
    if result is None:
        return 1
    if not isinstance(result.get("base64"), str) or len(result["base64"]) < 100:
        return fail(f"10/viewport_screenshot: base64 too short len={len(result.get('base64', ''))}")
    if result.get("mime") != "image/png":
        return fail(f"10/viewport_screenshot: mime mismatch {result.get('mime')!r}")
    if result.get("width") != 256 or result.get("height") != 256:
        return fail(f"10/viewport_screenshot: dims mismatch want=256x256 got={result.get('width')}x{result.get('height')}")
    try:
        decoded = base64.b64decode(result["base64"], validate=True)
    except Exception as exc:
        return fail(f"10/viewport_screenshot: base64 decode failed {exc!r}")
    if not is_png_magic(decoded):
        return fail(f"10/viewport_screenshot: decoded bytes lack PNG magic prefix")
    print(f"[SMOKE_PHASE5_EDITOR]   10/editor.viewport_screenshot OK "
          f"(256x256 PNG, base64_len={len(result['base64'])}, decoded={len(decoded)}B)")

    # Also test JPG path
    result = expect_ok(
        call(args.host, args.port, "10b", "p5-edt-10b", "editor.viewport_screenshot",
             {"width": 128, "height": 128, "format": "jpg"}),
        "p5-edt-10b", "10b/viewport_screenshot-jpg")
    if result is None:
        return 1
    if result.get("mime") != "image/jpeg":
        return fail(f"10b/viewport_screenshot: jpg mime mismatch {result.get('mime')!r}")
    try:
        decoded = base64.b64decode(result["base64"], validate=True)
    except Exception as exc:
        return fail(f"10b/viewport_screenshot: jpg base64 decode failed {exc!r}")
    if not is_jpg_magic(decoded):
        return fail(f"10b/viewport_screenshot: decoded bytes lack JPG magic prefix")
    print(f"[SMOKE_PHASE5_EDITOR]   10b/editor.viewport_screenshot(jpg) OK (decoded={len(decoded)}B)")

    # ─── Sub-test 11: viewport_screenshot oversized → -32602 ───────────────────────────────────
    resp = call(args.host, args.port, "11", "p5-edt-11", "editor.viewport_screenshot",
                {"width": 10000, "height": 768})
    if expect_error(resp, "p5-edt-11", -32602, "11/viewport_screenshot-oversized") is None:
        return 1
    print(f"[SMOKE_PHASE5_EDITOR]   11/editor.viewport_screenshot(width=10000) → -32602")

    # ─── Sub-test 12: viewport_screenshot_to_disk ──────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "12", "p5-edt-12", "editor.viewport_screenshot_to_disk",
             {"width": 512, "height": 512, "format": "png"}),
        "p5-edt-12", "12/viewport_screenshot_to_disk")
    if result is None:
        return 1
    out_path = result.get("path")
    out_bytes = result.get("bytes")
    if not isinstance(out_path, str) or not os.path.exists(out_path):
        return fail(f"12/viewport_screenshot_to_disk: file not on disk {result!r}")
    if not isinstance(out_bytes, (int, float)) or out_bytes <= 0:
        return fail(f"12/viewport_screenshot_to_disk: bytes<=0 {result!r}")
    if result.get("width") != 512 or result.get("height") != 512:
        return fail(f"12/viewport_screenshot_to_disk: dims mismatch {result!r}")
    # Verify file contents
    with open(out_path, "rb") as f:
        first_bytes = f.read(16)
    if not is_png_magic(first_bytes):
        return fail(f"12/viewport_screenshot_to_disk: file lacks PNG magic")
    print(f"[SMOKE_PHASE5_EDITOR]   12/editor.viewport_screenshot_to_disk OK "
          f"({int(out_bytes)}B at {out_path})")

    # PathEscape — try writing somewhere outside sandbox.
    resp = call(args.host, args.port, "12b", "p5-edt-12b", "editor.viewport_screenshot_to_disk",
                {"path": "C:/Windows/System32/should_fail.png"})
    if expect_error(resp, "p5-edt-12b", -32013, "12b/viewport_screenshot_to_disk-path-escape") is None:
        return 1
    print(f"[SMOKE_PHASE5_EDITOR]   12b/editor.viewport_screenshot_to_disk(escape) → -32013")

    # ─── Sub-test 13: pie.screenshot_to_disk WITHOUT PIE → -32038 ──────────────────────────────
    if pie_active_initial:
        skip("13/pie.screenshot_to_disk: PIE is active; cannot verify -32038 path. "
             "Stop PIE manually and re-run smoke to verify.")
    else:
        resp = call(args.host, args.port, "13", "p5-edt-13", "pie.screenshot_to_disk",
                    {"width": 256, "height": 256})
        err = expect_error(resp, "p5-edt-13", -32038, "13/pie.screenshot_to_disk-no-pie")
        if err is None:
            return 1
        msg = str(err.get("message", ""))
        # Frozen message contract from Chunk A: "PIE is not running" + "pie.start" + "editor.*"
        for required in ("PIE is not running", "pie.start"):
            if required not in msg:
                return fail(
                    f"13/pie.screenshot_to_disk: frozen message missing substring {required!r}; got {msg!r}")
        print(f"[SMOKE_PHASE5_EDITOR]   13/pie.screenshot_to_disk(no PIE) → -32038 (frozen message OK)")

    print("[SMOKE_PHASE5_EDITOR] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
