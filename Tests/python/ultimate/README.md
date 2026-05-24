# ULTIMATE test suite

Comprehensive acceptance + crash-safety + concurrency + protocol + stability
+ security tests for `UnrealMCPBridge`, organized into 10 categories
(`A`..`J`) per `D:/tmp/ws3_stress/ULTIMATE_TEST_PLAN.md`.

## Layout

- `mcp_test_harness.py` — shared infra: `call`, `health`, `discover_methods*`,
  `TestLogger`, `ConnectionPool`, transport fuzzers. Imported by every phase
  script.
- `phase_<id>_<name>.py` — one script per phase. Each is self-contained,
  exits 0 on PASS, 1 on FAIL, 2 on editor-died.
- Logs land in `D:/tmp/ws3_stress/test_logs/` as `<phase>_<name>.md`
  (human-readable) + `<phase>_<name>.json` (machine).

## Pre-flight

1. Editor must be running with the MCP bridge listening on `127.0.0.1:30020`
   (the bridge auto-starts on editor launch).
2. From repo root:
   ```
   PYTHONIOENCODING=utf-8 python Plugins/UnrealMCPBridge/Tests/python/ultimate/phase_a1_inventory.py
   ```
3. Each script self-tests harness liveness on import; aborts cleanly if editor
   is unreachable.

## Status (post-S+16, HEAD 9c2244c)

Category A (functional baseline) shipped. Initial sweep against live editor
found a real crash (`asset.search_by_class` short class_path → UE
FTopLevelAssetPath ensure spam → editor destabilisation) that is fixed in
S+16. Subsequent runs are clean; no crashes through full A1+A2+A3+A4+A5+A6+A7
sweep.

| Phase | Script | LOC | Coverage | Result on first run |
|---|---|---|---|---|
| A1 | phase_a1_inventory.py | 220 | 431 methods × 1 dispatch each | 431/431 PASS |
| A2 | phase_a2_required_args.py | 280 | 312 methods × 3-5 hostile probes | 231/0F/3X (50-method run) |
| A3 | phase_a3_optional_defaults.py | 215 | 312 methods × 2 (minimal + extras) | 78/11F/0X (coverage gaps in A2's chain walker, 50-method run) |
| A4 | phase_a4_type_coercion.py | 220 | 312 methods × 2-3 coerce probes | 157/0F (50-method run) |
| A5 | phase_a5_roundtrip.py | 320 | 7 curated write→read pairs | 2P/1F/4X (X = arg-name mismatches in my hardcoded args; F = bp.create_blueprint param) |
| A6 | phase_a6_pagination.py | 240 | 19 paginated tools | 32P/0F/11X (11 tools rejected as not-paginated or missing) |
| A7 | phase_a7_error_codes.py | 230 | 17 documented error codes | 6P/3F/5X/3S (3F = behaviour findings: memreport.dump has no required mode; folder.create doesn't guard /Engine/; folder.create idempotent) |

## --limit N flag

A2/A3/A4 accept `--limit N` to restrict the run to the first N methods.
Useful for incremental testing or when the editor is slow under heavy load:

```
python phase_a2_required_args.py --limit 50
```

Without `--limit` the run covers all 312 methods (~30-40 min depending on
editor load).

## Findings

- **S+16**: `asset.search_by_class` + `MCPARFilterParser` triggered UE
  ensure spam on short class_paths. Fixed (separate commit).
- **A3 coverage gaps (11)**: methods where A2's chain walker missed
  some required fields because dummy values triggered handler-side
  validation before all fields were enumerated. Documented in
  `D:/tmp/ws3_stress/test_logs/a3_coverage_gaps.json`.
- **A7 findings**: `memreport.dump` has no required `mode` (claims it
  in tool comment but accepts empty args); `folder.create` does not
  reject `/Engine/...` (only `cb.*` mutators are mount-guarded);
  `folder.create` is silently idempotent (no `-32014 PathInUse` on
  re-create).
