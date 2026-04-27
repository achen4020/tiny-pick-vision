# v2 Device Acceptance — Lenovo TB322FC

Date: 2026-04-27
Device: Lenovo TB322FC (Android 16 / arm64-v8a, model TB322FC)
APK: `app-debug.apk` from commit `d88e038` (T-v2.8 head)
libtpv.so SHA-256: `12f2c15646944ff00c4874e7f3c8743c6ce76beeb83cb0b412a9c9d829cb003a`
model_data SHA-256: `b69b7cd83e9c48b5f7956f8520bae474089934200b455725b7a62acae6351a98` (stub — see Notes)

## Spec §9 criteria

| # | Criterion | Result | Evidence |
|---|---|---|---|
| 1 | Smoke — green mask paints, commits fire | PASS (software) | bin/CCL/winner/mask pipeline self-consistent on device; status_line, Diag panel, commit flash, HUD all render correctly. End-to-end coverage limited by stub model + physical reflectance — see Notes. |
| 2 | v1 replay parity | PASS | `run_2026-04-24T06:18:51Z.zip` (no `ui_version` → auto v1 mode); `log.jsonl` event #1 frame_idx=8 (cls=255, x=319, y=233, theta=808) ≡ `000001.y` CSV row. |
| 3 | v2 replay parity (9 fields match) | PASS | `run_2026-04-27T01:17:08Z.zip`; jsonl event #1 frame_idx=25 → CSV row 000001.y matches all 9 fields: status=0, cls=255, x=562, y=439, theta=-243, conf=0, area=5951, grid=130. |
| 4 | Mask visualization (shape matches winner) | PASS | `tools/visualize_mask.py /tmp/v2run/000001.mask` reports 5951 fg px (1.94%) ≡ jsonl `area_px=5951`; white pixels concentrated at bbox (496,375,144,105), matches commit-frame winner location. |
| 5 | Diagnostics panel (6 cells) | PASS | On-device screenshot shows raw Y / ROI / bin / all blobs / winner / last event with correct labels; toggling Diag button shows/hides panel as expected. |
| 6 | A2 p95 ≤ 10 ms | PASS | `python3 tools/analyze_timing.py /tmp/v2run/timing.bin` over 631 frames: **p50=3.26 ms, p95=4.02 ms, p99=4.73 ms**. v1 baseline was 4.84 ms; v2 is actually slightly faster on this device. |
| 7 | ROI exclusion (no event when obj outside ROI) | PASS | Settings → ROI=0,0,100,100; phone placed at frame center; State remained `EMPTY`, Events counter stayed at 0 for ≥5 s. ROI restored to 0,0,640,480 afterwards. |

## Notes

### Calibration is still pending
`src/model_data.c` is the stub (`tpv_templates = {0}`, model_data sha `b69b7c…` matches the stub-zero file). Every classified blob therefore returns `class_id=255` (`TPV_CLASS_REJECTED`). Commit triggers because `TriggerMachine` only requires geometric stability + drift, not a non-rejected class. Production runs will need `tools/calibrate` against real per-class scenes; the dark_object_mode at calibration time must match the runtime setting (see DEVELOPER.md §11 v2 upgrade note).

### Mask-coverage caveat (criterion 1, end-to-end)
On printed/glossy objects (CS:GO mouse pad with reflective ink, glossy phone glass), the binarization-only pipeline produces fragmented or partial masks because the object surface contains both Y < threshold (dark) and Y > threshold (highlights) regions. Diag confirmed the v2 pipeline is *self-consistent* — bin → CCL → winner → mask each step accurately reflects its input — so this is a physical/algorithmic limitation, not a v2 software regression. Identical behavior reproduces on v1.

Tested binThreshold values:
- 128 (default): partial coverage, frequent commits
- 120: best stability/coverage tradeoff for white-paper-bg-dark-obj scene
- 180: too permissive — paper shadows enter foreground, true blob drowned in noise

Future work (out of v2 scope): morphological close (dilate→erode) on `bin` would merge highlight-induced holes inside the object. Would add cost and bin/winner divergence; defer until calibration-phase data shows it's needed.

### Runtime parameters captured
v2 run `run_2026-04-27T01:17:08Z` ran with: `bin_threshold=120, dark_object_mode=true, roi=(0,0,640,480)`, n_classes=5 (stub). 631 total frames, 10 skipped, 1 committed event. All five v2 meta keys (`ui_version, tpv.bin_threshold, tpv.dark_object_mode, tpv.roi.{x,y,w,h}`) written and round-trip through replay.

### A2 timing budget
Spec §9 set v2 target at p95 ≤ 10 ms (tighter than v1's 30 ms gate, in case the new memset(3 × 38400) + grid_8x8 work added cost). Actual p95 4.02 ms shows the added per-frame work is negligible on this device class.
