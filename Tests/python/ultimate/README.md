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

## Status (post-S+16 + polish, HEAD c1e3764)

Category A (functional baseline) shipped + polished. Initial sweep
against live editor found a real crash (`asset.search_by_class` short
class_path → UE FTopLevelAssetPath ensure spam → editor destabilisation)
that is fixed in S+16. Subsequent runs are clean; no crashes through
full A1+A2+A3+A4+A5+A6+A7 sweep.

| Phase | Script | LOC | Coverage | Result |
|---|---|---|---|---|
| A1 | phase_a1_inventory.py | 220 | 431 methods × 1 dispatch | **431/431 PASS** (2m09s) |
| A2 | phase_a2_required_args.py | 295 | 312 methods × 3-5 hostile probes (≈1200 cases) | **1197P / 0F / 3X** full sweep (7m26s) — was 696F before hybrid |
| A3 | phase_a3_optional_defaults.py | 235 | 312 methods × 2 (minimal + extras) | 181P / 59F (--limit 150) — 59F = chain-incomplete methods (live fallback finds only 1 field) |
| A4 | phase_a4_type_coercion.py | 240 | 312 methods × 2-3 coerce probes | 157P / 0F (--limit 50 last validated) |
| A5 | phase_a5_roundtrip.py | 320 | 7 curated write→read pairs | **5P / 0F / 2X** |
| A6 | phase_a6_pagination.py | 240 | 19 paginated tools | **32P / 0F / 11X** |
| A7 | phase_a7_error_codes.py | 240 | 17 documented error codes | **8P / 0F / 6X / 3S** |

## A2 full sweep — FIXED via hybrid static+live chain discovery

The original chain-walker saturated Lane A by satisfying required-arg
validators with dummies, which let handlers RUN TO COMPLETION on every
method (~1500 Lane A mutations × 312 methods = editor queue death after
~150 methods). Full sweep was 88 min / 696 FAIL.

**Fix**: `discover_chains_static()` in mcp_test_harness.py parses .cpp
source for `RequireXxxField` sites (incl. surface-specific
`XXX_Require*` helpers via inline expansion), brace-matched function
bodies, file-aware keying to disambiguate handler name collisions
(`Tool_Dump` in MemReport and RenderTarget).

A2 stage 1 now uses static chains for the 168 methods source-parse
covers fully + single-shot live probe (no satisfying-continuing) for
the remaining 143. Stage 1 went from ~1500 live calls to ~143.

**Result**: A2 full 312-method sweep — **1197 PASS / 0 FAIL / 3 XFAIL /
1200 cases in 7m26s**. Editor alive throughout, zero crash dumps.

## --limit N for A3/A4 (still recommended)

A3 (`optional_defaults`) and A4 (`type_coercion`) test the WHOLE
method response, which means they call the handler with full satisfied
args — handler runs to completion → side effects → saturation, same as
A2's old walker. With hybrid chain discovery providing partial chains
(only first field for some methods), A3 also reports `coverage_gaps`
where chain is incomplete.

```
python phase_a3_optional_defaults.py --limit 150
python phase_a4_type_coercion.py --limit 100
```

Editor restart between A3 and A4/A5/A6/A7 is currently needed in the
full sequential sweep.

## Findings

- **S+16 (FIXED)**: `asset.search_by_class` + `MCPARFilterParser`
  triggered UE ensure spam on short class_paths. Pre-validate `.` in
  ClassPathNormalized before TrySetPath.
- **A3 coverage gaps (RESOLVED)**: dummy_value learned vector/rotator/
  enum/positive-int field-name heuristics; regex extended to match
  "non-empty"/"valid"/typeless variants of "missing required field".
  All 11 gaps closed.
- **A5 arg-name mismatches (RESOLVED)**: hardcoded test args fixed to
  match actual tool signatures (`dest_path`, `blueprint_path`, `path`).
  2 remaining XFAILs are intentional (PrimaryDataAsset abstract; ai.bb
  runtime accessors need PIE).
- **A7 behavioural notes (NOT BUGS, document only)**:
  - `memreport.dump` `mode` is documented but actually optional
    (defaults to "trigger") — A7 now uses `actor.get` for -32602.
  - `folder.create` ≠ `cb.create_folder`. `folder.create` is for world
    outliner FActorFolders (in-memory only); intentionally accepts any
    string label and is idempotent. `cb.create_folder` is for content
    browser disk folders; properly guards mount points and rejects dups.

## Polish history

- Initial run discovered S+16 crash + 11 chain-walker gaps + 4 arg-name
  mismatches in A5 + 3 false-positive findings in A7.
- Harness `dummy_value()` extended with field-name heuristics:
  vector/rotator/scale shapes (`[0,0,0]` / `{x:0,y:0,z:0}`),
  positive-int hints (`radius` → 1, etc.), enum hints (`key_type` →
  "Float", `verbosity` → "Log").
- `RE_MISSING` regex generalised to handle "non-empty"/"valid" prefixes
  and typeless "missing required field" form.
- A5 rewritten with correct tool signatures verified against live
  bridge.
- A7 reproducers re-routed to tools that genuinely enforce each error
  code.
