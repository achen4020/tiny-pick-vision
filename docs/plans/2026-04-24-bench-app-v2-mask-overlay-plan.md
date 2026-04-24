# Android Bench Test APP v2 — Mask Overlay + Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add mask-export to the tpv C library under `-DTPV_DEBUG_FEATURES`, plus an Android UI that paints the blob mask as a green translucent overlay on live preview, shows size/bbox/grid/rotation status text, supports ROI + dark-object threshold, adds a 6-cell diagnostic panel, records `.mask` per event, and extends `build/replay` with a v2 mode that reads parameters from `meta.json`.

**Architecture:** C side adds `tpv_process_frame_debug_v2()` that takes `bin_threshold` / `dark_object_mode` / ROI at runtime and emits three bitmaps (`bin`, `all_blobs_mask`, `mask`) alongside v1's detection fields. `tpv_ccl_moments` gains an optional `labels_out` parameter so the debug path can map CCL labels back to mask pixels; production `tpv_process_frame` passes NULL — 20 KB size gate and HG1-5 tests stay green. Kotlin upstream replaces `TpvDetectionDebug` with `TpvDetectionDebugV2` everywhere it flows through; OverlayView repaints green mask + red center + yellow ROI rect; new DiagnosticsView renders 6 derivation bitmaps; MainActivity wires run-locked Settings with 3 tabs.

**Tech Stack:** Existing — NDK r27+ (`aarch64-linux-android24-clang`), AGP 8.5, Kotlin 1.9.22, CameraX 1.3.4, JDK 17, JUnit4 for JVM unit tests. No new deps.

**Source of truth:** `docs/specs/2026-04-24-bench-app-v2-mask-overlay-design.md` — every field name, hex color, and test fixture pulled from there.

---

## File Structure

### C library (tpv) — additive only

```
include/tpv_internal.h            MODIFY — add tpv_DetectionDebugV2 + tpv_process_frame_debug_v2 decl + labels_out on tpv_ccl_moments
src/ccl_moments.c                 MODIFY — optional labels_out fill
src/pipeline.c                    MODIFY — pass NULL to tpv_ccl_moments in tpv_process_frame; add threshold_v2 + debug_v2 function
tests/test_debug_api_v2.c         NEW    — 5 JVM-style host tests w/ -DTPV_DEBUG_FEATURES
Makefile                          MODIFY — add test_debug_api_v2 rule
```

### Android app

```
android/app/src/main/java/com/tpv/bench/
  TpvNative.kt                    MODIFY — add TpvBbox, TpvDetectionDebugV2, processFrameDebugV2 external
  TriggerMachine.kt               MODIFY — type upgrade TpvDetectionDebug → TpvDetectionDebugV2
  RunRecorder.kt                  MODIFY — recordEvent v2: .mask file + bbox/areaPx/grid8x8/ui_version
  OverlayPainter.kt               MODIFY — maskToBitmap helper + ROI + green argb const
  OverlayView.kt                  MODIFY — LiveState carries TpvDetectionDebugV2 + roi; onDraw repainted
  DiagnosticsRenderer.kt          NEW    — pure logic: TpvDetectionDebugV2 → 6 Bitmaps (JVM-testable)
  DiagnosticsView.kt              NEW    — Android View, 2×3 layout, delegates to Renderer
  SettingsState.kt                MODIFY — binThreshold, darkObjectMode, roi{X,Y,W,H}
  CameraAdapter.kt                (unchanged)
  Yuv420ToNv21.kt                 (unchanged)
  MainActivity.kt                 MODIFY — snapshot v2 params, call processFrameDebugV2, status line, Diag/ROI/Clear buttons, 3-tab Settings

android/app/src/main/cpp/
  tpv_jni.c                       MODIFY — add processFrameDebugV2 JNI function + v2 jclass/jmethodID cache

android/app/src/main/res/layout/activity_main.xml   MODIFY — add Diag/ROI/Clear buttons + status TextView

android/app/src/test/java/com/tpv/bench/
  TriggerMachineTest.kt           MODIFY — helpers migrate to v2 type
  RunRecorderTest.kt              MODIFY — existing tests updated for v2 fields + new tests for .mask / v2 meta
  OverlayPainterTest.kt           MODIFY — add maskToBitmap tests
  DiagnosticsRendererTest.kt      NEW    — 6-cell rendering tests
```

### Tools (host)

```
tools/replay.c                    MODIFY — meta.json reading, --v1/--v2/--bin-threshold/--dark-object-mode/--roi flags, v2 CSV columns
tools/visualize_mask.py           NEW    — parse .mask → PNG
```

### Docs

```
docs/DEVELOPER.md                 MODIFY — §11.x "v2 mode" subsection
```

---

## Shared Type Contract (used across all tasks, do not drift)

**Kotlin data classes:**

```kotlin
data class TpvBbox(val x: Int, val y: Int, val w: Int, val h: Int)

data class TpvDetectionDebugV2(
    val det: TpvDetection,              // same as v1
    val features: TpvFeatures,          // same as v1
    val distancesSq: IntArray,          // same as v1, length TPV_N_CLASSES
    val bbox: TpvBbox,                  // winning blob bbox in 640×480
    val areaPx: Int,                    // winning blob m00
    val grid8x8: Int,                   // foreground cells in 80×60 grid (OR-aggregated)
    val bin: ByteArray,                 // 38400 B LSB-first bitmap, post-threshold+ROI
    val allBlobsMask: ByteArray,        // 38400 B, subset of bin (CCL succeeded only)
    val mask: ByteArray,                // 38400 B, subset of allBlobsMask (winning blob only)
) {
    override fun equals(other: Any?) = other is TpvDetectionDebugV2 &&
        det == other.det && features == other.features &&
        distancesSq.contentEquals(other.distancesSq) &&
        bbox == other.bbox && areaPx == other.areaPx && grid8x8 == other.grid8x8 &&
        bin.contentEquals(other.bin) &&
        allBlobsMask.contentEquals(other.allBlobsMask) &&
        mask.contentEquals(other.mask)
    override fun hashCode() =
        det.hashCode() * 31 + features.hashCode() * 31 +
        distancesSq.contentHashCode() * 31 + bbox.hashCode() * 31 +
        areaPx * 31 + grid8x8 * 31 +
        bin.contentHashCode() * 31 +
        allBlobsMask.contentHashCode() * 31 +
        mask.contentHashCode()
}
```

**JNI method signature:**

```kotlin
external fun processFrameDebugV2(
    y: ByteArray, width: Int, height: Int,
    binThreshold: Int,             // 0..255
    darkObjectMode: Boolean,       // true = Y < threshold is foreground
    roiX: Int, roiY: Int, roiW: Int, roiH: Int,
    outTimingNs: LongArray,        // size 3, same semantics as v1
): TpvDetectionDebugV2
```

**C type + function:**

```c
typedef struct {
    tpv_Detection det;
    tpv_Features  features;
    int32_t       distances_sq[TPV_N_CLASSES];
    int16_t  bbox_x0, bbox_y0, bbox_x1, bbox_y1;
    int32_t  area_px;
    int32_t  grid_8x8;
    uint8_t  bin[TPV_WIDTH * TPV_HEIGHT / 8];
    uint8_t  all_blobs_mask[TPV_WIDTH * TPV_HEIGHT / 8];
    uint8_t  mask[TPV_WIDTH * TPV_HEIGHT / 8];
} tpv_DetectionDebugV2;

int tpv_process_frame_debug_v2(
    const uint8_t *y, int w, int h,
    uint8_t bin_threshold,
    int dark_object_mode,
    int roi_x, int roi_y, int roi_w, int roi_h,
    tpv_DetectionDebugV2 *out);

/* Modified — adds optional labels_out */
int tpv_ccl_moments(const uint8_t *bin, int w, int h,
                    tpv_Blob *blobs_out, int max_blobs,
                    uint16_t *labels_out);   /* NULL = skip label fill */
```

**SettingsSnapshot (MainActivity private):**

```kotlin
private data class SettingsSnapshot(
    val n: Int, val k: Int, val m: Int,
    val binThreshold: Int,
    val darkObjectMode: Boolean,
    val roiX: Int, val roiY: Int, val roiW: Int, val roiH: Int,
)
```

**Constants (color, dimensions):**

- `OverlayPainter.GREEN_MASK_ARGB = 0x7800FF00.toInt()` (alpha ≈ 120)
- `OverlayPainter.YELLOW_ROI_ARGB = 0xFFF5A623.toInt()`
- `OverlayPainter.RED_CENTER_ARGB = 0xFFD0021B.toInt()`
- `MASK_BYTES = 640 * 480 / 8 = 38400`

---

## Task 1: C side — `tpv_process_frame_debug_v2` + labels_out + 5 host tests

**Goal:** Add the v2 debug entry point that takes threshold / dark_mode / roi as runtime args, emits three bitmaps (`bin`, `all_blobs_mask`, `mask`), and exposes CCL label map via optional `tpv_ccl_moments(..., uint16_t *labels_out)` parameter. All existing v1 tests stay green; size gate unchanged.

**Files:**
- Modify: `include/tpv_internal.h` (add type + 2 declarations; change `tpv_ccl_moments` signature)
- Modify: `src/ccl_moments.c` (honour new `labels_out` param)
- Modify: `src/pipeline.c` (pass `NULL` from prod path; add `threshold_v2` + `tpv_process_frame_debug_v2`)
- Create: `tests/test_debug_api_v2.c`
- Modify: `Makefile` (register new test with `-DTPV_DEBUG_FEATURES`)

---

- [ ] **Step 1.1: Extend `include/tpv_internal.h` with v2 type + declarations + modified `tpv_ccl_moments` signature**

Find the existing `int tpv_ccl_moments(const uint8_t *bin, int w, int h, tpv_Blob *blobs_out, int max_blobs);` declaration and replace with:

```c
int tpv_ccl_moments(const uint8_t *bin, int w, int h,
                    tpv_Blob *blobs_out, int max_blobs,
                    uint16_t *labels_out);
```

Then append at the very end (inside the existing `#ifdef TPV_DEBUG_FEATURES` block if one exists, else create one):

```c
#ifdef TPV_DEBUG_FEATURES
typedef struct {
    tpv_Detection det;
    tpv_Features  features;
    int32_t       distances_sq[TPV_N_CLASSES];

    int16_t  bbox_x0, bbox_y0, bbox_x1, bbox_y1;
    int32_t  area_px;
    int32_t  grid_8x8;
    uint8_t  bin[TPV_WIDTH * TPV_HEIGHT / 8];
    uint8_t  all_blobs_mask[TPV_WIDTH * TPV_HEIGHT / 8];
    uint8_t  mask[TPV_WIDTH * TPV_HEIGHT / 8];
} tpv_DetectionDebugV2;

int tpv_process_frame_debug_v2(
    const uint8_t *y, int w, int h,
    uint8_t bin_threshold,
    int dark_object_mode,
    int roi_x, int roi_y, int roi_w, int roi_h,
    tpv_DetectionDebugV2 *out);
#endif  /* TPV_DEBUG_FEATURES */
```

- [ ] **Step 1.2: Update `src/pipeline.c`'s call to `tpv_ccl_moments` to pass NULL**

Find the line `int n = tpv_ccl_moments(g_bin, w, h, g_blobs, TPV_MAX_BLOBS);` in `tpv_process_frame` and change to:

```c
    int n = tpv_ccl_moments(g_bin, w, h, g_blobs, TPV_MAX_BLOBS, NULL);
```

This is the only change inside the production `tpv_process_frame`. Behavior is unchanged (labels_out ignored when NULL).

- [ ] **Step 1.3: Update `src/ccl_moments.c` implementation to honour `labels_out`**

Find `int tpv_ccl_moments(const uint8_t *bin, int w, int h, tpv_Blob *blobs_out, int max_blobs) {` and change signature:

```c
int tpv_ccl_moments(const uint8_t *bin, int w, int h,
                    tpv_Blob *blobs_out, int max_blobs,
                    uint16_t *labels_out) {
```

Find where final compact labels are determined (`rl = g_remap[g_labels[idx]]`). The debug `labels_out` buffer must contain the same compact labels that index `blobs_out[rl - 1]`; do **not** copy pre-remap union-find labels.

```c
            uint16_t rl = g_labels[idx] ? g_remap[g_labels[idx]] : 0;
            if (labels_out) labels_out[idx] = rl;
```

Place this in pass 2 after union-find compaction, immediately after `rl` is computed for each pixel. Also make sure background pixels get `labels_out[idx] = 0`. This guarantees `winner_label = winner_blob + 1` in Step 1.4 maps to the same blob index used for moments/bbox.

- [ ] **Step 1.4: Append `threshold_v2` helper + `tpv_process_frame_debug_v2` to `src/pipeline.c`**

At the very end of `src/pipeline.c`, inside a fresh `#ifdef TPV_DEBUG_FEATURES` block (or the existing one from v1's `tpv_process_frame_debug`):

```c
#ifdef TPV_DEBUG_FEATURES

/* v2 extras: runtime-tunable threshold + direction, ROI clipping, and
 * per-frame label map buffer for mask derivation. All static + #ifdef-
 * guarded; production build sees none of this. */
static uint16_t g_labels_v2[TPV_WIDTH * TPV_HEIGHT];

static void threshold_v2(const uint8_t *y, int w, int h,
                          uint8_t threshold, int dark_mode,
                          uint8_t *bin_out) {
    const int npix = w * h;
    const int nby  = (npix + 7) / 8;
    memset(bin_out, 0, nby);
    for (int i = 0; i < npix; i++) {
        int is_fg = dark_mode ? (y[i] < threshold) : (y[i] >= threshold);
        if (is_fg) bin_out[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}

static void clip_bin_to_roi(uint8_t *bin, int w, int h,
                             int roi_x, int roi_y, int roi_w, int roi_h) {
    for (int yy = 0; yy < h; yy++) {
        int in_row = (yy >= roi_y && yy < roi_y + roi_h);
        if (in_row) {
            /* Clear columns outside [roi_x, roi_x+roi_w) */
            for (int xx = 0; xx < roi_x; xx++) {
                int i = yy * w + xx;
                bin[i >> 3] &= (uint8_t)~(1u << (i & 7));
            }
            for (int xx = roi_x + roi_w; xx < w; xx++) {
                int i = yy * w + xx;
                bin[i >> 3] &= (uint8_t)~(1u << (i & 7));
            }
        } else {
            for (int xx = 0; xx < w; xx++) {
                int i = yy * w + xx;
                bin[i >> 3] &= (uint8_t)~(1u << (i & 7));
            }
        }
    }
}

static void compute_grid_8x8(const uint8_t *mask, int w, int h, int32_t *out) {
    int grid_w = w / 8, grid_h = h / 8;
    int count = 0;
    for (int gy = 0; gy < grid_h; gy++) {
        for (int gx = 0; gx < grid_w; gx++) {
            /* Is any pixel in this 8×8 block foreground? */
            int any = 0;
            for (int dy = 0; dy < 8 && !any; dy++) {
                for (int dx = 0; dx < 8 && !any; dx++) {
                    int x = gx * 8 + dx, y = gy * 8 + dy;
                    int i = y * w + x;
                    if (mask[i >> 3] & (1u << (i & 7))) any = 1;
                }
            }
            if (any) count++;
        }
    }
    *out = count;
}

int tpv_process_frame_debug_v2(
    const uint8_t *y, int w, int h,
    uint8_t bin_threshold,
    int dark_object_mode,
    int roi_x, int roi_y, int roi_w, int roi_h,
    tpv_DetectionDebugV2 *out)
{
    if (!out) return TPV_BAD_INPUT;
    memset(out, 0, sizeof *out);

    if (!y || w != TPV_WIDTH || h != TPV_HEIGHT) return TPV_BAD_INPUT;
    if (roi_x < 0 || roi_y < 0 || roi_w <= 0 || roi_h <= 0 ||
        roi_x + roi_w > w || roi_y + roi_h > h) return TPV_BAD_INPUT;

    /* 1. threshold_v2 with runtime params (NOT tpv_bin_threshold global) */
    threshold_v2(y, w, h, bin_threshold, dark_object_mode, out->bin);

    /* 2. ROI clip — writes out->bin in place */
    clip_bin_to_roi(out->bin, w, h, roi_x, roi_y, roi_w, roi_h);

    /* 3. CCL with label map */
    static tpv_Blob blobs[TPV_MAX_BLOBS];
    int n = tpv_ccl_moments(out->bin, w, h, blobs, TPV_MAX_BLOBS, g_labels_v2);
    if (n < 0) {
        /* Non-OK contract: BAD_INPUT / SCENE_ERROR / EMPTY expose no stale
         * diagnostic masks. Clear bin too, even though thresholding already ran. */
        memset(out, 0, sizeof *out);
        return TPV_SCENE_ERROR;
    }

    /* 4. Fill all_blobs_mask: every non-zero label = 1 */
    const int npix = w * h;
    for (int i = 0; i < npix; i++) {
        if (g_labels_v2[i] != 0) {
            out->all_blobs_mask[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }

    /* 5. Geometric + classification — same decision as v1
     * To avoid duplicating tpv_process_frame's whole body, the cleanest
     * path is to call it with a pre-built bin. But v1 tpv_process_frame
     * re-thresholds from scratch. For v2 we duplicate the filter/pose/
     * classify/argmax logic here, referencing blobs[] we just computed. */
    static tpv_Detection pool[TPV_MAX_BLOBS];
    static int32_t       d1_pool[TPV_MAX_BLOBS];
    static tpv_Features  feat_pool[TPV_MAX_BLOBS];
    static int           blob_idx_pool[TPV_MAX_BLOBS];
    int pn = 0;
    for (int i = 0; i < n; i++) {
        if (blobs[i].m00 < TPV_AMIN || blobs[i].m00 > TPV_AMAX) continue;
        tpv_Features f;
        tpv_shape_features(&blobs[i], &f);
        tpv_Detection d = {0};
        uint8_t cid = 0, conf = 0; int32_t d1sq = 0;
        tpv_classify(&f, tpv_templates, TPV_N_CLASSES, &cid, &conf, &d1sq);
        tpv_pose(&blobs[i], &d.x, &d.y, &d.theta_x10);
        d.class_id = cid; d.confidence_q8 = conf;
        pool[pn] = d;
        d1_pool[pn] = d1sq;
        feat_pool[pn] = f;
        blob_idx_pool[pn] = i;
        pn++;
    }
    if (pn == 0) {
        memset(out, 0, sizeof *out);
        return TPV_EMPTY;
    }

    /* argmax over ACCEPTED, else min d1 over REJECTED/AMBIGUOUS */
    int best_acc = -1, best_conf = -1;
    int best_reject = -1; int32_t best_d1 = INT32_MAX;
    for (int i = 0; i < pn; i++) {
        if (pool[i].class_id <= 4) {
            if (pool[i].confidence_q8 > best_conf) {
                best_conf = pool[i].confidence_q8;
                best_acc = i;
            }
        } else {
            if (d1_pool[i] < best_d1) {
                best_d1 = d1_pool[i];
                best_reject = i;
            }
        }
    }
    int winner_pn = (best_acc >= 0) ? best_acc
                  : (best_reject >= 0) ? best_reject
                  : -1;
    if (winner_pn < 0) {
        memset(out, 0, sizeof *out);
        return TPV_EMPTY;
    }

    int winner_blob = blob_idx_pool[winner_pn];
    /* labels_out contains compact labels aligned with blobs_out[rl - 1]. */
    uint16_t winner_label = (uint16_t)(winner_blob + 1);

    out->det = pool[winner_pn];
    out->features = feat_pool[winner_pn];
    for (int c = 0; c < TPV_N_CLASSES; c++) {
        int64_t d = tpv_mahal_sq_q16(&out->features, &tpv_templates[c]);
        out->distances_sq[c] = (int32_t)(d > INT32_MAX ? INT32_MAX : d);
    }

    out->bbox_x0 = blobs[winner_blob].bbox_x0;
    out->bbox_y0 = blobs[winner_blob].bbox_y0;
    out->bbox_x1 = blobs[winner_blob].bbox_x1;
    out->bbox_y1 = blobs[winner_blob].bbox_y1;
    out->area_px = (int32_t)blobs[winner_blob].m00;

    /* 6. Fill winning mask: pixels with label == winner_label */
    for (int i = 0; i < npix; i++) {
        if (g_labels_v2[i] == winner_label) {
            out->mask[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }

    /* 7. grid_8x8 from mask */
    compute_grid_8x8(out->mask, w, h, &out->grid_8x8);

    return TPV_OK;
}

#endif  /* TPV_DEBUG_FEATURES */
```

If `tpv_shape_features`, `tpv_pose`, `tpv_classify`, or `tpv_mahal_sq_q16` prototypes aren't already visible in `pipeline.c` (they should be via `tpv_internal.h`), add the includes. `TPV_AMIN` / `TPV_AMAX` / `TPV_MAX_BLOBS` come from `tpv_config.h` which `tpv_internal.h` already pulls in.

- [ ] **Step 1.5: Create `tests/test_debug_api_v2.c`**

```c
/* Build with -DTPV_DEBUG_FEATURES. Verifies the v2 debug entry, the
 * three-layer mask inclusion invariant, dark-object-mode semantics, ROI
 * clipping, and v1 ↔ v2 decision parity. */
#include <string.h>
#include "tpv.h"
#include "tpv_internal.h"
#include "testlib.h"

static uint8_t frame[TPV_WIDTH * TPV_HEIGHT];

/* Count set bits in an LSB-first packed bitmap. */
static int popcount_bits(const uint8_t *b, int nbytes) {
    int c = 0;
    for (int i = 0; i < nbytes; i++) c += __builtin_popcount((unsigned)b[i]);
    return c;
}

static int bit_subset(const uint8_t *sub, const uint8_t *super, int nbytes) {
    for (int i = 0; i < nbytes; i++) {
        if ((sub[i] & ~super[i]) != 0) return 0;
    }
    return 1;
}

static void paint_bright_square(int cx, int cy, int half) {
    memset(frame, 0, sizeof frame);
    for (int y = cy - half; y < cy + half; y++)
        for (int x = cx - half; x < cx + half; x++)
            frame[y * TPV_WIDTH + x] = 255;
}

static void paint_dark_square_on_bright_bg(int cx, int cy, int half) {
    memset(frame, 200, sizeof frame);
    for (int y = cy - half; y < cy + half; y++)
        for (int x = cx - half; x < cx + half; x++)
            frame[y * TPV_WIDTH + x] = 30;
}

TEST(t_v2_bright_square_matches_v1_decision) {
    paint_bright_square(320, 240, 30);

    tpv_Detection prod;
    int rc1 = tpv_process_frame(frame, TPV_WIDTH, TPV_HEIGHT, &prod);

    tpv_DetectionDebugV2 v2;
    int rc2 = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT,
        /* bin_threshold */ 128,
        /* dark_object_mode */ 0,
        /* roi */ 0, 0, TPV_WIDTH, TPV_HEIGHT,
        &v2);

    CHECK_EQ_I(rc1, rc2);
    CHECK_EQ_I(prod.class_id, v2.det.class_id);
    CHECK_EQ_I(prod.x, v2.det.x);
    CHECK_EQ_I(prod.y, v2.det.y);
    CHECK_EQ_I(prod.theta_x10, v2.det.theta_x10);
    CHECK_EQ_I(prod.confidence_q8, v2.det.confidence_q8);
}

TEST(t_v2_dark_object_mode_inverts_threshold) {
    /* White background Y=200, 40×40 dark (Y=30) object at (320, 240) */
    paint_dark_square_on_bright_bg(320, 240, 20);

    /* dark_mode=0: white background > 128 → foreground, ~307200-1600 px > AMAX → rejected
     * dark object < 128 → background → not seen → TPV_EMPTY */
    tpv_DetectionDebugV2 v2a;
    int rc_a = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 0,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2a);
    CHECK_EQ_I(rc_a, TPV_EMPTY);

    /* dark_mode=1: white background > 128 → background, dark object < 128 → foreground
     * 40×40 = 1600 px ∈ [AMIN=500, AMAX=50000] → TPV_OK */
    tpv_DetectionDebugV2 v2b;
    int rc_b = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 1,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2b);
    CHECK_EQ_I(rc_b, TPV_OK);
    CHECK(v2b.area_px >= 1500 && v2b.area_px <= 1700);
}

TEST(t_v2_roi_clips_outside_blobs) {
    /* Bright 60×60 square centered at (320, 240). ROI at top-left only. */
    paint_bright_square(320, 240, 30);

    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 0,
        /* roi = (0,0,50,50) — nowhere near (320,240) */
        0, 0, 50, 50, &v2);
    CHECK_EQ_I(rc, TPV_EMPTY);
}

TEST(t_v2_mask_matches_detection_area) {
    paint_bright_square(320, 240, 30);
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 0,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    /* 60×60 = 3600 pixels; CCL should detect ~3600 ± small boundary rounding */
    int n_mask = popcount_bits(v2.mask, sizeof v2.mask);
    CHECK(n_mask >= v2.area_px - 5 && n_mask <= v2.area_px + 5);
    CHECK(v2.area_px >= 3500 && v2.area_px <= 3700);
}

TEST(t_v2_mask_allblobs_bin_inclusion) {
    paint_bright_square(320, 240, 30);
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 0,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    /* Three-layer inclusion: mask ⊆ all_blobs_mask ⊆ bin */
    CHECK(bit_subset(v2.mask, v2.all_blobs_mask, sizeof v2.mask));
    CHECK(bit_subset(v2.all_blobs_mask, v2.bin, sizeof v2.bin));
}

int main(void) {
    RUN(t_v2_bright_square_matches_v1_decision);
    RUN(t_v2_dark_object_mode_inverts_threshold);
    RUN(t_v2_roi_clips_outside_blobs);
    RUN(t_v2_mask_matches_detection_area);
    RUN(t_v2_mask_allblobs_bin_inclusion);
    FINISH();
}
```

- [ ] **Step 1.6: Register `test_debug_api_v2` in `Makefile`**

Locate the existing `build/test_debug_api:` rule (added in v1 T1) and the `TEST_FILES = $(filter-out tests/test_debug_api.c,$(wildcard tests/test_*.c))` line. Replace with:

```make
TEST_FILES = $(filter-out tests/test_debug_api.c tests/test_debug_api_v2.c,$(wildcard tests/test_*.c))
TEST_BINS  = $(patsubst tests/%.c,build/%,$(TEST_FILES)) build/test_debug_api build/test_debug_api_v2
```

And add the explicit rule right below the v1 `build/test_debug_api` rule:

```make
build/test_debug_api_v2: tests/test_debug_api_v2.c tests/testlib.c $(SRCS) tests/stub_model_data.c | build
	$(CC_HOST) $(CFLAGS_HOST) -DTPV_DEBUG_FEATURES -o $@ $^ -lm
```

- [ ] **Step 1.7: Build only the new test, expect link failure**

Run: `make build/test_debug_api_v2`

Expected: something like `Undefined symbols: _tpv_process_frame_debug_v2` (the declaration is in the header, but nothing has been linked — wait, we added the impl in 1.4, so link may succeed. If it does, skip to 1.8. If Steps 1.1–1.4 are split across commits, this interim step lets you verify the failing-test → passing-test cycle on just the new function.)

Practically: run `make clean && make build/test_debug_api_v2 && ./build/test_debug_api_v2 2>&1 | tail -10`.

- [ ] **Step 1.8: Confirm new test passes**

Run: `make clean && make build/test_debug_api_v2 && ./build/test_debug_api_v2`

Expected last lines:
```
  ok  t_v2_bright_square_matches_v1_decision
  ok  t_v2_dark_object_mode_inverts_threshold
  ok  t_v2_roi_clips_outside_blobs
  ok  t_v2_mask_matches_detection_area
  ok  t_v2_mask_allblobs_bin_inclusion
5 passed, 0 failed
```

- [ ] **Step 1.9: Run full suite, expect 39/39**

Run: `make clean && make test 2>&1 | tail -30`

Expected: 10 existing binaries + 1 new (`test_debug_api_v2`) all green; grand total 34 + 5 = 39 tests passing.

- [ ] **Step 1.10: Production size gate**

Run with NDK on PATH: `make clean && make size 2>&1 | tail -3`

Expected: `OK: <bytes> ≤ 20480`, with `<bytes>` close to the v1 baseline 13632 B (predicted increment < 50 B per spec §4.5).

If the NDK isn't on PATH, note it and skip — the controller verifies this.

- [ ] **Step 1.11: Commit**

```bash
git add include/tpv_internal.h src/ccl_moments.c src/pipeline.c \
        tests/test_debug_api_v2.c Makefile
git commit -m "$(cat <<'EOF'
feat(tpv): v2 debug API — mask/bin/all_blobs + runtime threshold/ROI

tpv_process_frame_debug_v2 takes bin_threshold, dark_object_mode, and
an ROI rect as runtime args, returning a tpv_DetectionDebugV2 that
carries v1's Detection/Features/distances plus three 38400 B bitmaps
(bin post-threshold+ROI, all_blobs after successful CCL, mask for the
winning blob) and winner bbox/area_px/grid_8x8 derived features.

tpv_ccl_moments grows an optional uint16_t *labels_out param; production
tpv_process_frame passes NULL (one-line change, verified size gate).
The debug function reuses the labels to derive all_blobs_mask and mask.

5 host tests (tests/test_debug_api_v2.c) cover: v1 parity on bright
square, dark_object_mode inversion on a white-bg-dark-object frame, ROI
clipping, popcount(mask) == area_px ± 5, and the mask ⊆ all_blobs ⊆ bin
inclusion invariant. Full suite 39/39; size gate unchanged.
EOF
)"
```

---

## Task 2: JNI + Kotlin data class wiring

**Goal:** Expose `tpv_process_frame_debug_v2` to Kotlin as `TpvNative.processFrameDebugV2(...)` returning a populated `TpvDetectionDebugV2`. v1 `processFrameDebug` stays valid (used nowhere after Task 6 lands, but left in the code for API stability and future v1-mode `build/replay`).

**Files:**
- Modify: `android/app/src/main/java/com/tpv/bench/TpvNative.kt` (add `TpvBbox`, `TpvDetectionDebugV2`, `processFrameDebugV2 external`)
- Modify: `android/app/src/main/cpp/tpv_jni.c` (add JniCacheV2 + `Java_com_tpv_bench_TpvNative_processFrameDebugV2`)

---

- [ ] **Step 2.1: Add `TpvBbox` + `TpvDetectionDebugV2` to `TpvNative.kt`**

Open `android/app/src/main/java/com/tpv/bench/TpvNative.kt`. At the end of the file (after the v1 `TpvDetectionDebug` data class, before `object TpvNative { ... }` — or right before the `object` declaration; order matters for JNI class lookup by name only, not by file order, so this is aesthetic):

```kotlin
data class TpvBbox(val x: Int, val y: Int, val w: Int, val h: Int)

data class TpvDetectionDebugV2(
    val det: TpvDetection,
    val features: TpvFeatures,
    val distancesSq: IntArray,
    val bbox: TpvBbox,
    val areaPx: Int,
    val grid8x8: Int,
    val bin: ByteArray,
    val allBlobsMask: ByteArray,
    val mask: ByteArray,
) {
    override fun equals(other: Any?) = other is TpvDetectionDebugV2 &&
        det == other.det && features == other.features &&
        distancesSq.contentEquals(other.distancesSq) &&
        bbox == other.bbox && areaPx == other.areaPx && grid8x8 == other.grid8x8 &&
        bin.contentEquals(other.bin) &&
        allBlobsMask.contentEquals(other.allBlobsMask) &&
        mask.contentEquals(other.mask)
    override fun hashCode(): Int {
        var h = det.hashCode()
        h = h * 31 + features.hashCode()
        h = h * 31 + distancesSq.contentHashCode()
        h = h * 31 + bbox.hashCode()
        h = h * 31 + areaPx
        h = h * 31 + grid8x8
        h = h * 31 + bin.contentHashCode()
        h = h * 31 + allBlobsMask.contentHashCode()
        h = h * 31 + mask.contentHashCode()
        return h
    }
}
```

- [ ] **Step 2.2: Add `processFrameDebugV2` external in `TpvNative`**

Inside `object TpvNative { ... }`, after the existing `external fun processFrameDebug(...)`, add:

```kotlin
/**
 * Run the v2 debug variant with runtime-tunable threshold / dark mode / ROI.
 * Returns a TpvDetectionDebugV2 whose `bin`, `allBlobsMask`, and `mask` are
 * each 38400 bytes (640×480 / 8, LSB-first packed).
 *
 * @param binThreshold  0..255 cutoff; dark_object_mode determines polarity.
 * @param darkObjectMode true = Y < threshold is foreground; false = Y ≥ threshold.
 * @param roiX..roiH     ROI rect in 640×480 coords. Use (0,0,640,480) to disable.
 * @param outTimingNs    same semantics as v1: [jni_enter, tpv_enter, tpv_exit].
 */
external fun processFrameDebugV2(
    y: ByteArray, width: Int, height: Int,
    binThreshold: Int,
    darkObjectMode: Boolean,
    roiX: Int, roiY: Int, roiW: Int, roiH: Int,
    outTimingNs: LongArray,
): TpvDetectionDebugV2
```

- [ ] **Step 2.3: Extend `tpv_jni.c` with v2 class/method cache + v2 JNI function**

Open `android/app/src/main/cpp/tpv_jni.c`. After the existing `static JniCache s_cache;` definition, add a parallel cache struct + static:

```c
typedef struct {
    jclass bbox_cls, dbg_v2_cls;
    jmethodID bbox_ctor, dbg_v2_ctor;
    int initialized;
} JniCacheV2;
static JniCacheV2 s_cache_v2;

static int init_cache_v2(JNIEnv *env) {
    if (s_cache_v2.initialized) return 0;
    /* v1 cache must already be initialized (TpvDetection/TpvFeatures exist). */
    if (init_cache(env) < 0) return -1;

    jclass bbox = (*env)->FindClass(env, "com/tpv/bench/TpvBbox");
    jclass dbg = (*env)->FindClass(env, "com/tpv/bench/TpvDetectionDebugV2");
    if (!bbox || !dbg) {
        LOGE("FindClass v2 failed");
        return -1;
    }
    s_cache_v2.bbox_cls   = (*env)->NewGlobalRef(env, bbox);
    s_cache_v2.dbg_v2_cls = (*env)->NewGlobalRef(env, dbg);

    /* TpvBbox(x, y, w, h): Int */
    s_cache_v2.bbox_ctor = (*env)->GetMethodID(env, s_cache_v2.bbox_cls, "<init>",
        "(IIII)V");
    /* TpvDetectionDebugV2(
     *     det: TpvDetection, features: TpvFeatures, distancesSq: IntArray,
     *     bbox: TpvBbox, areaPx: Int, grid8x8: Int,
     *     bin: ByteArray, allBlobsMask: ByteArray, mask: ByteArray) */
    s_cache_v2.dbg_v2_ctor = (*env)->GetMethodID(env, s_cache_v2.dbg_v2_cls, "<init>",
        "(Lcom/tpv/bench/TpvDetection;"
        "Lcom/tpv/bench/TpvFeatures;"
        "[I"
        "Lcom/tpv/bench/TpvBbox;"
        "II"
        "[B[B[B)V");

    if (!s_cache_v2.bbox_ctor || !s_cache_v2.dbg_v2_ctor) {
        LOGE("GetMethodID v2 failed");
        (*env)->DeleteGlobalRef(env, s_cache_v2.bbox_cls);
        (*env)->DeleteGlobalRef(env, s_cache_v2.dbg_v2_cls);
        s_cache_v2.bbox_cls = s_cache_v2.dbg_v2_cls = NULL;
        return -1;
    }
    s_cache_v2.initialized = 1;
    return 0;
}

/* v2 uses its own static output buffer so the 115 KB struct is not alloc'd
 * on the stack. Single-threaded camera callback makes this safe. */
static tpv_DetectionDebugV2 s_v2_out;

JNIEXPORT jobject JNICALL
Java_com_tpv_bench_TpvNative_processFrameDebugV2(
    JNIEnv *env, jobject thiz, jbyteArray y, jint w, jint h,
    jint bin_threshold, jboolean dark_object_mode,
    jint roi_x, jint roi_y, jint roi_w, jint roi_h,
    jlongArray out_timing_ns)
{
    jlong t_jni_enter = monotonic_ns();

    if (init_cache_v2(env) < 0) {
        throw_illegal_state(env, "tpv_jni: v2 cache init failed");
        return NULL;
    }

    const jsize n = (jsize)(w * h);
    if (n <= 0 || n > (jsize)sizeof s_frame_buf) {
        throw_illegal_state(env, "tpv_jni: Y buffer size out of bounds");
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, y, 0, n, (jbyte *)s_frame_buf);

    jlong t_tpv_enter = monotonic_ns();
    int rc = tpv_process_frame_debug_v2(
        s_frame_buf, w, h,
        (uint8_t)(bin_threshold & 0xFF),
        dark_object_mode ? 1 : 0,
        (int)roi_x, (int)roi_y, (int)roi_w, (int)roi_h,
        &s_v2_out);
    jlong t_tpv_exit = monotonic_ns();

    jlong times[3] = { t_jni_enter, t_tpv_enter, t_tpv_exit };
    (*env)->SetLongArrayRegion(env, out_timing_ns, 0, 3, times);

    (void)rc;  /* caller inspects det.status (== rc) */

    /* Build v1 det + features + distances objects (reuse v1 cache) */
    jobject det_obj = (*env)->NewObject(env, s_cache.det_cls, s_cache.det_ctor,
        (jint)rc,
        (jint)(uint32_t)s_v2_out.det.class_id,
        (jint)s_v2_out.det.x, (jint)s_v2_out.det.y,
        (jint)s_v2_out.det.theta_x10,
        (jint)(uint32_t)s_v2_out.det.confidence_q8);

    jintArray hu = (*env)->NewIntArray(env, 7);
    (*env)->SetIntArrayRegion(env, hu, 0, 7, (const jint *)s_v2_out.features.hu);
    jobject feat_obj = (*env)->NewObject(env, s_cache.feat_cls, s_cache.feat_ctor,
        hu,
        (jint)s_v2_out.features.perim_ratio,
        (jint)s_v2_out.features.eccentricity,
        (jint)s_v2_out.features.m3_axis_sign);

    jintArray dsq = (*env)->NewIntArray(env, TPV_N_CLASSES);
    (*env)->SetIntArrayRegion(env, dsq, 0, TPV_N_CLASSES,
        (const jint *)s_v2_out.distances_sq);

    /* v2-only objects */
    jobject bbox_obj = (*env)->NewObject(env, s_cache_v2.bbox_cls, s_cache_v2.bbox_ctor,
        (jint)s_v2_out.bbox_x0,
        (jint)s_v2_out.bbox_y0,
        (jint)(s_v2_out.bbox_x1 - s_v2_out.bbox_x0),
        (jint)(s_v2_out.bbox_y1 - s_v2_out.bbox_y0));

    const jsize MASK_LEN = TPV_WIDTH * TPV_HEIGHT / 8;
    jbyteArray bin_arr = (*env)->NewByteArray(env, MASK_LEN);
    (*env)->SetByteArrayRegion(env, bin_arr, 0, MASK_LEN, (const jbyte *)s_v2_out.bin);
    jbyteArray all_arr = (*env)->NewByteArray(env, MASK_LEN);
    (*env)->SetByteArrayRegion(env, all_arr, 0, MASK_LEN, (const jbyte *)s_v2_out.all_blobs_mask);
    jbyteArray mask_arr = (*env)->NewByteArray(env, MASK_LEN);
    (*env)->SetByteArrayRegion(env, mask_arr, 0, MASK_LEN, (const jbyte *)s_v2_out.mask);

    return (*env)->NewObject(env, s_cache_v2.dbg_v2_cls, s_cache_v2.dbg_v2_ctor,
        det_obj, feat_obj, dsq,
        bbox_obj, (jint)s_v2_out.area_px, (jint)s_v2_out.grid_8x8,
        bin_arr, all_arr, mask_arr);
}
```

- [ ] **Step 2.4: Build APK**

Run (with NDK + platform-tools on PATH):
```bash
make android-apk 2>&1 | tail -10
```

Expected: `BUILD SUCCESSFUL`. The native lib `libtpv_jni.so` should now export `Java_com_tpv_bench_TpvNative_processFrameDebugV2` (verify: `llvm-readelf --dyn-syms android/app/build/intermediates/cxx/Debug/*/obj/arm64-v8a/libtpv_jni.so | grep processFrame`).

- [ ] **Step 2.5: Host unit tests still green**

Run: `cd android && ./gradlew :app:testDebugUnitTest 2>&1 | tail -5`

Expected: still 31/31 (Task 2 does not add any Kotlin unit tests; it adds data classes and an external declaration — compiled but not called yet).

- [ ] **Step 2.6: Commit**

```bash
git add android/app/src/main/java/com/tpv/bench/TpvNative.kt \
        android/app/src/main/cpp/tpv_jni.c
git commit -m "feat(android): JNI bridge to tpv_process_frame_debug_v2

Adds TpvBbox + TpvDetectionDebugV2 Kotlin data classes and a
processFrameDebugV2(...) external that takes bin_threshold,
darkObjectMode, ROI, and outTimingNs. v1 processFrameDebug stays
valid for now; later tasks upgrade call sites to v2.

JNI side has a parallel JniCacheV2 for TpvBbox + TpvDetectionDebugV2
class/method lookup; init_cache_v2 depends on (and inherits) v1's
init_cache for TpvDetection/TpvFeatures. 115 KB tpv_DetectionDebugV2
is a module-static buffer filled in place; per-frame allocation is
only the 3 × 38400 B byte[] copies into Java heap."
```

---

## Task 3: RunRecorder v2 — .mask files + v2 jsonl fields + ui_version meta

**Goal:** `CommittedEvent` + `RunRecorder.recordEvent` accept the v2 detection struct; each event writes a `NNNNNN.mask` file (38400 B raw); `log.jsonl` gains `detection.bbox / detection.area_px / detection.grid_8x8 / artifacts.mask`; `meta.json` grows `ui_version: "v2"`, `tpv.bin_threshold`, `tpv.dark_object_mode`, `tpv.roi`. `TriggerMachine` and `FrameObservation` type-upgrade to use `TpvDetectionDebugV2`.

**Files:**
- Modify: `android/app/src/main/java/com/tpv/bench/TriggerMachine.kt` (type-upgrade only; no logic change)
- Modify: `android/app/src/main/java/com/tpv/bench/RunRecorder.kt` (MetaInfo fields, recordEvent signature, .mask write, jsonl new fields)
- Modify: `android/app/src/test/java/com/tpv/bench/TriggerMachineTest.kt` (helper `dummyDebug` returns v2)
- Modify: `android/app/src/test/java/com/tpv/bench/RunRecorderTest.kt` (existing tests assert new fields; 2 new v2-specific tests)

---

- [ ] **Step 3.1: Type-upgrade `FrameObservation` and `CommittedEvent` in `TriggerMachine.kt`**

Find in `android/app/src/main/java/com/tpv/bench/TriggerMachine.kt`:

```kotlin
data class FrameObservation(
    val presence: FramePresence,
    val x: Int, val y: Int,
    val classId: Int,
    val frameIdxInRun: Long,
    val detection: TpvDetectionDebug?
)

data class CommittedEvent(
    val eventIdx: Long,
    val triggerFrameIdx: Long,
    val triggerFrameDebug: TpvDetectionDebug,
    val eventClassId: Int,
    val classIdHistogram: Map<Int, Int>,
    val flicker: Boolean,
)
```

Change both references `TpvDetectionDebug` → `TpvDetectionDebugV2`. No other changes in this file; the state machine logic doesn't touch any v2-specific field.

- [ ] **Step 3.2: Update `TriggerMachineTest.kt`'s `dummyDebug`/`makeDetection`/`present` helpers**

Find the `makeDetection` helper that currently returns `TpvDetectionDebug`:

```kotlin
private fun makeDetection(classId: Int, x: Int = 320, y: Int = 240) =
    TpvDetectionDebug(
        det = TpvDetection(
            status = 0, classId = classId,
            x = x, y = y, thetaX10 = 0, confidenceQ8 = if (classId < 5) 200 else 0
        ),
        features = TpvFeatures(IntArray(7), 0, 0, 0),
        distancesSq = IntArray(5)
    )
```

Change to:

```kotlin
private val EMPTY_MASK = ByteArray(640 * 480 / 8)
private fun makeDetection(classId: Int, x: Int = 320, y: Int = 240) =
    TpvDetectionDebugV2(
        det = TpvDetection(
            status = 0, classId = classId,
            x = x, y = y, thetaX10 = 0, confidenceQ8 = if (classId < 5) 200 else 0
        ),
        features = TpvFeatures(IntArray(7), 0, 0, 0),
        distancesSq = IntArray(5),
        bbox = TpvBbox(x - 10, y - 10, 20, 20),
        areaPx = 400,
        grid8x8 = 7,
        bin = EMPTY_MASK,
        allBlobsMask = EMPTY_MASK,
        mask = EMPTY_MASK,
    )
```

- [ ] **Step 3.3: Run TriggerMachine tests, confirm all green**

Run: `cd android && ./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.TriggerMachineTest" 2>&1 | tail -5`

Expected: 13/13 pass (type upgrade is transparent to state-machine logic).

- [ ] **Step 3.4: Extend `MetaInfo` in `RunRecorder.kt` with v2 fields**

Find the `data class MetaInfo(` declaration and add three fields after `binThreshold`:

```kotlin
data class MetaInfo(
    val runId: String,
    val deviceModel: String, val androidLevel: Int, val abi: String, val cpuMaxFreqKhz: Long,
    val soSha256: String, val modelDataSha256: String,
    val nClasses: Int, val binThreshold: Int,
    // v2 additions
    val darkObjectMode: Boolean,
    val roiX: Int, val roiY: Int, val roiW: Int, val roiH: Int,
    // unchanged tail
    val nStable: Int, val kEmpty: Int, val mDriftPx: Int,
    val requestedW: Int, val requestedH: Int,
    val nativeW: Int, val nativeH: Int,
    val cropX: Int, val cropY: Int, val cropW: Int, val cropH: Int,
    val downsampleRatioX: Double, val downsampleRatioY: Double,
)
```

- [ ] **Step 3.5: Extend `metaToJson()` in `RunRecorder.kt` with v2 fields**

Find `private fun metaToJson(): JSONObject`. Insert into the `tpv` object:

```kotlin
val tpv = JSONObject()
    .put("so_sha256", meta.soSha256)
    .put("model_data_sha256", meta.modelDataSha256)
    .put("n_classes", meta.nClasses)
    .put("bin_threshold", meta.binThreshold)
    .put("dark_object_mode", meta.darkObjectMode)         // NEW
    .put("roi", JSONObject()                               // NEW
        .put("x", meta.roiX).put("y", meta.roiY)
        .put("w", meta.roiW).put("h", meta.roiH))
```

At the top level of the returned `JSONObject`, add `ui_version`:

```kotlin
return JSONObject()
    .put("run_id", meta.runId)
    .put("ui_version", "v2")           // NEW
    .put("device", device)
    .put("tpv", tpv)
    .put("trigger", trigger)
    .put("camera", camera)
```

- [ ] **Step 3.6: Extend `recordEvent` signature + body with `.mask` file + new jsonl fields**

Find `fun recordEvent(event: CommittedEvent, triggerTsMs: Long, rawY: ByteArray, overlayJpeg: ByteArray)` and change to:

```kotlin
fun recordEvent(
    event: CommittedEvent, triggerTsMs: Long,
    rawY: ByteArray, overlayJpeg: ByteArray,
    mask: ByteArray,            // NEW: size must be 640*480/8 = 38400
) {
    val w = logWriter ?: error("RunRecorder.start() not called")
    val ls = logStream ?: error("RunRecorder.start() not called")

    val name = "%06d".format(event.eventIdx)
    File(runDir, "$name.y").writeBytes(rawY)
    File(runDir, "$name.jpg").writeBytes(overlayJpeg)
    File(runDir, "$name.mask").writeBytes(mask)                  // NEW

    val d = event.triggerFrameDebug
    val det = JSONObject()
        .put("status", d.det.status)
        .put("class_id", d.det.classId)
        .put("x", d.det.x).put("y", d.det.y)
        .put("theta_x10", d.det.thetaX10)
        .put("confidence_q8", d.det.confidenceQ8)
        // v2 additions
        .put("bbox", JSONObject()
            .put("x", d.bbox.x).put("y", d.bbox.y)
            .put("w", d.bbox.w).put("h", d.bbox.h))
        .put("area_px", d.areaPx)
        .put("grid_8x8", d.grid8x8)

    val huArr = JSONArray()
    for (v in d.features.hu) huArr.put("0x%08x".format(v))
    val feat = JSONObject()
        .put("hu", huArr)
        .put("perim_ratio",  "0x%08x".format(d.features.perimRatio))
        .put("eccentricity", "0x%08x".format(d.features.eccentricity))
        .put("m3_axis_sign", d.features.m3AxisSign)

    val dsq = JSONArray()
    for (v in d.distancesSq) dsq.put(v)

    val hist = JSONObject()
    for ((k, v) in event.classIdHistogram) hist.put(k.toString(), v)

    val artifacts = JSONObject()
        .put("raw_y", "$name.y")
        .put("overlay", "$name.jpg")
        .put("mask", "$name.mask")              // NEW

    val line = JSONObject()
        .put("event_idx", event.eventIdx)
        .put("trigger_ts_ms", triggerTsMs)
        .put("frame_idx_in_run", event.triggerFrameIdx)
        .put("detection", det)
        .put("event_class_id", event.eventClassId)
        .put("class_id_histogram", hist)
        .put("flicker", event.flicker)
        .put("features", feat)
        .put("distances_sq", dsq)
        .put("artifacts", artifacts)

    w.write(line.toString())
    w.newLine()
    w.flush()
    ls.fd.sync()
}
```

- [ ] **Step 3.7: Update existing `RunRecorderTest.kt` helpers for v2**

Find the `dummyDebug(classId: Int)` helper:

```kotlin
private fun dummyDebug(classId: Int) = TpvDetectionDebug(
    det = TpvDetection(0, classId, 320, 240, -450, 200),
    features = TpvFeatures(IntArray(7) { 0x0001_0000 * it }, 0x1a2b4, 0xdd74, 0),
    distancesSq = intArrayOf(12345678, 987654, 456789, 111111, 999999)
)
```

Replace with:

```kotlin
private fun dummyDebug(classId: Int) = TpvDetectionDebugV2(
    det = TpvDetection(0, classId, 320, 240, -450, 200),
    features = TpvFeatures(IntArray(7) { 0x0001_0000 * it }, 0x1a2b4, 0xdd74, 0),
    distancesSq = intArrayOf(12345678, 987654, 456789, 111111, 999999),
    bbox = TpvBbox(265, 180, 110, 139),
    areaPx = 15290,
    grid8x8 = 150,
    bin = ByteArray(640 * 480 / 8),
    allBlobsMask = ByteArray(640 * 480 / 8),
    mask = ByteArray(640 * 480 / 8).also { it[0] = 0x0F },  // 4 bits set for assertions
)
```

Find the `meta()` helper and extend with v2 fields:

```kotlin
private fun meta() = MetaInfo(
    runId = "run_test", deviceModel = "Pixel-test", androidLevel = 34,
    abi = "arm64-v8a", cpuMaxFreqKhz = 2_800_000,
    soSha256 = "a".repeat(64), modelDataSha256 = "b".repeat(64),
    nClasses = 5, binThreshold = 137,
    darkObjectMode = true,
    roiX = 0, roiY = 0, roiW = 640, roiH = 480,
    nStable = 3, kEmpty = 5, mDriftPx = 30,
    requestedW = 640, requestedH = 480,
    nativeW = 1280, nativeH = 720,
    cropX = 160, cropY = 0, cropW = 960, cropH = 720,
    downsampleRatioX = 1.5, downsampleRatioY = 1.5
)
```

Find the existing `recordEvent writes y file, jpg file, and jsonl line` test. Update its call site to pass a mask, and add assertions for the new fields:

```kotlin
@Test
fun `recordEvent writes y file, jpg file, mask file, and jsonl line`() {
    val rec = RunRecorder(tmp.root, meta())
    rec.start()
    val rawY = ByteArray(640 * 480) { (it and 0xFF).toByte() }
    val jpg = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0xFF.toByte(), 0xD9.toByte())
    val mask = ByteArray(640 * 480 / 8).also { it[100] = 0x5A; it[200] = 0x7F }
    val ev = CommittedEvent(
        eventIdx = 1,
        triggerFrameIdx = 42,
        triggerFrameDebug = dummyDebug(254),
        eventClassId = 2,
        classIdHistogram = mapOf(2 to 2, 254 to 1),
        flicker = true
    )
    rec.recordEvent(ev, triggerTsMs = 1_745_394_128_012L,
        rawY = rawY, overlayJpeg = jpg, mask = mask)
    rec.close()

    val yFile = File(tmp.root, "000001.y")
    assertEquals(640 * 480L, yFile.length())
    assertArrayEquals(rawY, yFile.readBytes())

    val jpgFile = File(tmp.root, "000001.jpg")
    assertArrayEquals(jpg, jpgFile.readBytes())

    val maskFile = File(tmp.root, "000001.mask")
    assertEquals((640 * 480 / 8).toLong(), maskFile.length())
    assertArrayEquals(mask, maskFile.readBytes())

    val line = File(tmp.root, "log.jsonl").readLines().single()
    val j = JSONObject(line)
    assertEquals(1, j.getInt("event_idx"))
    assertEquals(42, j.getInt("frame_idx_in_run"))
    assertEquals(2, j.getInt("event_class_id"))
    assertEquals(true, j.getBoolean("flicker"))
    val det = j.getJSONObject("detection")
    assertEquals(254, det.getInt("class_id"))
    assertEquals(15290, det.getInt("area_px"))
    assertEquals(150, det.getInt("grid_8x8"))
    val bbox = det.getJSONObject("bbox")
    assertEquals(265, bbox.getInt("x"))
    assertEquals(180, bbox.getInt("y"))
    assertEquals(110, bbox.getInt("w"))
    assertEquals(139, bbox.getInt("h"))
    val artifacts = j.getJSONObject("artifacts")
    assertEquals("000001.y", artifacts.getString("raw_y"))
    assertEquals("000001.jpg", artifacts.getString("overlay"))
    assertEquals("000001.mask", artifacts.getString("mask"))
}
```

- [ ] **Step 3.8: Update `start writes meta json with all spec fields` test to assert v2 meta fields**

Inside that test, add after existing assertions:

```kotlin
    assertEquals("v2", j.getString("ui_version"))
    assertEquals(true, j.getJSONObject("tpv").getBoolean("dark_object_mode"))
    val roi = j.getJSONObject("tpv").getJSONObject("roi")
    assertEquals(0, roi.getInt("x"))
    assertEquals(640, roi.getInt("w"))
```

- [ ] **Step 3.9: Update `stopAndZip produces a zip containing all run files` test**

Change the `rec.recordEvent(...)` call to include the new `mask = ByteArray(640 * 480 / 8)` arg:

```kotlin
    rec.recordEvent(
        CommittedEvent(1, 5, dummyDebug(0), 0, mapOf(0 to 3), false),
        1L, rawY, jpg, mask = ByteArray(640 * 480 / 8)
    )
```

And add `"000001.mask"` to the assertTrue(entries.contains(...)) chain:

```kotlin
    assertTrue(entries.contains("000001.mask"))
```

- [ ] **Step 3.10: Run full Kotlin unit test suite**

Run: `cd android && ./gradlew :app:testDebugUnitTest 2>&1 | tail -10`

Expected: 31/31 + 0 new (the existing 5 RunRecorder tests now also cover v2 fields). Total stays 31/31 because all modifications were to existing tests.

- [ ] **Step 3.11: Commit**

```bash
git add android/app/src/main/java/com/tpv/bench/TriggerMachine.kt \
        android/app/src/main/java/com/tpv/bench/RunRecorder.kt \
        android/app/src/test/java/com/tpv/bench/TriggerMachineTest.kt \
        android/app/src/test/java/com/tpv/bench/RunRecorderTest.kt
git commit -m "feat(android): RunRecorder v2 — .mask file + bbox/area_px/grid_8x8 in jsonl

MetaInfo grows darkObjectMode + roi{X,Y,W,H}; meta.json adds
ui_version:\"v2\" and tpv.{dark_object_mode, roi} at the run level.

recordEvent signature gains a required `mask: ByteArray` parameter;
each event now writes NNNNNN.mask (38400 B raw bitmap). jsonl line
gains detection.bbox/area_px/grid_8x8 and artifacts.mask.

CommittedEvent.triggerFrameDebug and FrameObservation.detection both
type-upgrade to TpvDetectionDebugV2. TriggerMachine logic unchanged;
the new v2 fields flow through transparently.

Existing 5 RunRecorderTest cases updated to assert v2 fields; 13/13
TriggerMachine tests still green; 31/31 overall."
```

---

## Task 4: OverlayView v2 — green mask + red center + yellow ROI

**Goal:** Replace the v1 circle with a green translucent mask fill over the winning blob's pixels. Add a yellow ROI outline. Keep the red center dot + short axis line. Add a `maskToBitmap` helper in `OverlayPainter` with JVM unit tests.

**Files:**
- Modify: `android/app/src/main/java/com/tpv/bench/OverlayPainter.kt` (maskToBitmap helper + color constants)
- Modify: `android/app/src/main/java/com/tpv/bench/OverlayView.kt` (LiveState holds TpvDetectionDebugV2 + roi; onDraw repainted)
- Modify: `android/app/src/test/java/com/tpv/bench/OverlayPainterTest.kt` (maskToBitmap tests)

---

- [ ] **Step 4.1: Add color constants + maskToBitmap helper to `OverlayPainter.kt`**

At the top of `object OverlayPainter` (after existing constants like `AMBER_WARN`), add:

```kotlin
/** Mask fill color — pure green, alpha ≈ 120/255 ≈ 47% (spec §5.1). */
const val GREEN_MASK_ARGB = 0x7800FF00.toInt()
/** ROI rectangle stroke color — same amber used for AMBIGUOUS warning. */
const val YELLOW_ROI_ARGB = 0xFFF5A623.toInt()
/** Center dot color. */
const val RED_CENTER_ARGB = 0xFFD0021B.toInt()

/** 640×480 / 8 = 38400 bytes per mask. */
const val MASK_BYTES = 640 * 480 / 8
```

Then add the `maskToBitmap` helper as a member of `OverlayPainter`:

```kotlin
/**
 * Decode an LSB-first packed mask bitmap (width*height/8 bytes) into an
 * ARGB Bitmap where set bits → `argb`, clear bits → 0 (fully transparent).
 * Exposed at object level so unit tests can exercise it without Android.
 * Kotlin `Bitmap.createBitmap(IntArray, w, h, Config)` is provided by
 * `android.graphics.Bitmap`; on the JVM test side we isolate the pure
 * packing logic in `decodeMaskToArgb(...)`.
 */
fun decodeMaskToArgb(mask: ByteArray, w: Int, h: Int, argb: Int): IntArray {
    require(mask.size == w * h / 8) {
        "mask size ${mask.size} != expected ${w * h / 8}"
    }
    val out = IntArray(w * h)
    for (i in 0 until w * h) {
        val byte = mask[i shr 3].toInt() and 0xFF
        val bit = (byte ushr (i and 7)) and 1
        if (bit == 1) out[i] = argb
    }
    return out
}
```

A separate `maskToBitmap(mask, w, h, argb): android.graphics.Bitmap` wrapper lives inside `OverlayView.kt` where Android imports are OK; the pure-logic `decodeMaskToArgb` on `OverlayPainter` is what we unit test.

- [ ] **Step 4.2: Add `decodeMaskToArgb` tests to `OverlayPainterTest.kt`**

Append to the existing test class:

```kotlin
    @Test
    fun `decodeMaskToArgb zero mask yields all-transparent array`() {
        val mask = ByteArray(64 / 8)
        val out = OverlayPainter.decodeMaskToArgb(mask, 8, 8, 0xFF00FF00.toInt())
        assertEquals(64, out.size)
        for (v in out) assertEquals(0, v)
    }

    @Test
    fun `decodeMaskToArgb single bit at position 0 sets first pixel`() {
        val mask = ByteArray(64 / 8)
        mask[0] = 0x01  // bit 0 of byte 0 → pixel index 0
        val argb = 0xFF00FF00.toInt()
        val out = OverlayPainter.decodeMaskToArgb(mask, 8, 8, argb)
        assertEquals(argb, out[0])
        for (i in 1 until 64) assertEquals(0, out[i])
    }

    @Test
    fun `decodeMaskToArgb LSB-first byte 0 0xFF sets first 8 pixels`() {
        val mask = ByteArray(64 / 8)
        mask[0] = 0xFF.toByte()
        val argb = 0x7800FF00.toInt()
        val out = OverlayPainter.decodeMaskToArgb(mask, 8, 8, argb)
        for (i in 0 until 8) assertEquals(argb, out[i])
        for (i in 8 until 64) assertEquals(0, out[i])
    }

    @Test
    fun `decodeMaskToArgb rejects size mismatch`() {
        val bad = ByteArray(7)  // should be 8 for 8x8
        try {
            OverlayPainter.decodeMaskToArgb(bad, 8, 8, 0)
            fail("expected IllegalArgumentException")
        } catch (e: IllegalArgumentException) {
            assertTrue(e.message!!.contains("mask size"))
        }
    }
```

Add imports at the top if not present: `import org.junit.Assert.fail` and `import org.junit.Assert.assertTrue`.

- [ ] **Step 4.3: Upgrade `OverlayView.LiveState` to carry v2 detection + ROI**

Find `private data class LiveState(...)` and replace with:

```kotlin
private data class LiveState(
    val d: TpvDetectionDebugV2,
    val roi: YuvAdapter.CropRect,         // ROI in 640×480 coords
    val crop: YuvAdapter.CropRect,        // camera→640×480 crop
    val nativeW: Int, val nativeH: Int,
)
```

Change `updateLive` signature:

```kotlin
fun updateLive(
    d: TpvDetectionDebugV2,
    roi: YuvAdapter.CropRect,
    crop: YuvAdapter.CropRect,
    nativeW: Int, nativeH: Int,
) {
    live.set(LiveState(d, roi, crop, nativeW, nativeH))
    postInvalidate()
}
```

- [ ] **Step 4.4: Rewrite `onDraw` to paint mask + ROI + center + axis**

Replace the entire `onDraw` body:

```kotlin
override fun onDraw(canvas: Canvas) {
    val f = live.get() ?: return
    if (width == 0 || height == 0) return
    val vw = width.toFloat() ; val vh = height.toFloat()
    val sx = vw / f.nativeW ; val sy = vh / f.nativeH

    // ---- 1. Yellow ROI rectangle (in 640×480 coords → native → view) ----
    val (roi_nx0, roi_ny0) = OverlayPainter.mapCoord(f.roi.x, f.roi.y, f.crop)
    val (roi_nx1, roi_ny1) = OverlayPainter.mapCoord(
        f.roi.x + f.roi.w, f.roi.y + f.roi.h, f.crop)
    val roiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE ; strokeWidth = 2f
        this.color = OverlayPainter.YELLOW_ROI_ARGB
    }
    canvas.drawRect(roi_nx0 * sx, roi_ny0 * sy, roi_nx1 * sx, roi_ny1 * sy, roiPaint)

    // ---- 2. Green mask fill ----
    // Only draw mask if a blob was detected (status == TPV_OK, class_id set).
    // For TPV_EMPTY/DROP the mask is all zeros; skip allocation.
    val status = f.d.det.status
    if (status == 0) {
        val pixels = OverlayPainter.decodeMaskToArgb(
            f.d.mask, 640, 480, OverlayPainter.GREEN_MASK_ARGB)
        val maskBitmap = Bitmap.createBitmap(pixels, 640, 480, Bitmap.Config.ARGB_8888)
        // Destination: map 640×480 mask into crop rect on native, then to view.
        val dstLeft   = (f.crop.x * sx)
        val dstTop    = (f.crop.y * sy)
        val dstRight  = ((f.crop.x + f.crop.w) * sx)
        val dstBottom = ((f.crop.y + f.crop.h) * sy)
        val dstRect = android.graphics.RectF(dstLeft, dstTop, dstRight, dstBottom)
        canvas.drawBitmap(maskBitmap, null, dstRect, null)
    }

    // ---- 3. Red center dot + short axis line ----
    if (status == 0) {
        val (nx, ny) = OverlayPainter.mapCoord(f.d.det.x, f.d.det.y, f.crop)
        val cx = nx * sx ; val cy = ny * sy
        val dotR = (f.crop.w * 0.015f * sx).coerceAtLeast(4f)
        val centerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.FILL
            this.color = OverlayPainter.RED_CENTER_ARGB
        }
        canvas.drawCircle(cx, cy, dotR, centerPaint)

        val axisLen = (f.crop.w * 0.04f * sx).coerceAtLeast(8f)
        val thetaRad = Math.toRadians(f.d.det.thetaX10 / 10.0)
        val axisPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE ; strokeWidth = 3f
            this.color = OverlayPainter.RED_CENTER_ARGB
        }
        canvas.drawLine(
            cx, cy,
            (cx + axisLen * cos(thetaRad)).toFloat(),
            (cy + axisLen * sin(thetaRad)).toFloat(),
            axisPaint
        )
    }

    // ---- 4. Commit flash (persistent; overlay's pre-existing logic) ----
    val c = commit.get()
    if (c != null) {
        val nowMs = System.currentTimeMillis()
        val remaining = c.flashEndMs - nowMs
        if (remaining > 0) {
            flashPaint.alpha = (255 * remaining / 300).toInt().coerceIn(0, 255)
            canvas.drawRect(0f, 0f, vw, vh, flashPaint)
            postInvalidateDelayed(16)
        }
    }
}
```

Remove the v1 two-line text painting (textLine1/textLine2 draws from `onDraw`) — status text now lives in the HUD/status bar (Task 6). Keep `commit.get()` logic. Keep `clearLive()` / `onCommit()` / `reset()` unchanged.

- [ ] **Step 4.5: Update imports in `OverlayView.kt`**

Make sure these imports are at the top:

```kotlin
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import kotlin.math.cos
import kotlin.math.sin
```

Remove the old `import android.graphics.Color` if it was the only thing using it from v1's `Color.GREEN` (now `OverlayPainter.GREEN_MASK_ARGB` replaces it for mask).

- [ ] **Step 4.6: Run OverlayPainter tests (new ones pass)**

Run: `cd android && ./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.OverlayPainterTest" 2>&1 | tail -5`

Expected: 9 existing + 4 new = 13 tests pass.

- [ ] **Step 4.7: Build APK**

Run: `make android-apk 2>&1 | tail -5`

Expected: `BUILD SUCCESSFUL`. OverlayView's v2 compile requires `TpvDetectionDebugV2` (Task 2) — confirmed dependency satisfied.

- [ ] **Step 4.8: Commit**

```bash
git add android/app/src/main/java/com/tpv/bench/OverlayPainter.kt \
        android/app/src/main/java/com/tpv/bench/OverlayView.kt \
        android/app/src/test/java/com/tpv/bench/OverlayPainterTest.kt
git commit -m "feat(android): OverlayView v2 — green mask fill + yellow ROI + red dot

OverlayPainter gains decodeMaskToArgb (pure logic, 4 JVM tests) plus
GREEN_MASK_ARGB/YELLOW_ROI_ARGB/RED_CENTER_ARGB constants from spec §5.1.

OverlayView.onDraw is repainted: yellow stroke ROI rect + green
translucent bitmap over the blob mask (only when status==TPV_OK) +
red filled center dot + short axis line. v1's circle+text is dropped;
status text moves to the HUD in T6. Commit flash logic preserved.

LiveState carries TpvDetectionDebugV2 + roi; updateLive signature
matches. 13 OverlayPainter tests (9 old + 4 new) green; APK builds."
```

---

## Task 5: DiagnosticsView — 2×3 panel + 6-cell renderer

**Goal:** A new View that, when visible, displays 6 small bitmaps showing the tpv pipeline's intermediate stages. Pure rendering logic extracted to a JVM-testable `DiagnosticsRenderer`; the View is a thin Canvas wrapper.

**Files:**
- Create: `android/app/src/main/java/com/tpv/bench/DiagnosticsRenderer.kt`
- Create: `android/app/src/main/java/com/tpv/bench/DiagnosticsView.kt`
- Create: `android/app/src/test/java/com/tpv/bench/DiagnosticsRendererTest.kt`

---

- [ ] **Step 5.1: Create `DiagnosticsRenderer.kt` with pure-logic bitmap-pixel generation**

```kotlin
package com.tpv.bench

/**
 * Pure logic: turns TpvDetectionDebugV2 + latest raw Y frame + ROI into
 * six IntArray pixel arrays (ARGB), each downsampled to a fixed small tile
 * size. JVM-testable without Android. DiagnosticsView wraps these into
 * Bitmaps and lays them out on a Canvas.
 *
 * Output tile dimensions: 160×120 per cell (small enough to be cheap,
 * large enough to see shape). 6 cells laid out 2 rows × 3 cols.
 */
object DiagnosticsRenderer {

    const val TILE_W = 160
    const val TILE_H = 120

    /** Result of rendering: six tiles in documented order. */
    data class Panels(
        val rawY: IntArray,
        val roiCrop: IntArray,
        val binarized: IntArray,
        val allBlobs: IntArray,
        val winningBlob: IntArray,
        val lastEventMask: IntArray,
    )

    /**
     * @param rawY          current frame 640×480 Y buffer
     * @param d             latest detection (v2)
     * @param roi           ROI rect in 640×480 coords
     * @param lastEventMask last committed event's mask (null until first commit)
     */
    fun render(
        rawY: ByteArray,
        d: TpvDetectionDebugV2,
        roi: YuvAdapter.CropRect,
        lastEventMask: ByteArray?,
    ): Panels {
        require(rawY.size == 640 * 480) { "rawY must be 640*480 bytes" }

        val tileBytes = TILE_W * TILE_H

        // 1. Raw Y, downsampled to TILE_W × TILE_H, grayscale ARGB
        val rawTile = IntArray(tileBytes)
        for (ty in 0 until TILE_H) {
            val syIdx = (ty * 480) / TILE_H
            for (tx in 0 until TILE_W) {
                val sxIdx = (tx * 640) / TILE_W
                val v = rawY[syIdx * 640 + sxIdx].toInt() and 0xFF
                rawTile[ty * TILE_W + tx] = 0xFF shl 24 or (v shl 16) or (v shl 8) or v
            }
        }

        // 2. ROI crop: same as raw but with pixels outside ROI dimmed to 40%
        val roiTile = IntArray(tileBytes)
        for (ty in 0 until TILE_H) {
            val syIdx = (ty * 480) / TILE_H
            val inRow = (syIdx >= roi.y && syIdx < roi.y + roi.h)
            for (tx in 0 until TILE_W) {
                val sxIdx = (tx * 640) / TILE_W
                val v = rawY[syIdx * 640 + sxIdx].toInt() and 0xFF
                val inRoi = inRow && sxIdx >= roi.x && sxIdx < roi.x + roi.w
                val vv = if (inRoi) v else v * 40 / 100
                roiTile[ty * TILE_W + tx] = 0xFF shl 24 or (vv shl 16) or (vv shl 8) or vv
            }
        }

        // 3. Binarized: d.bin → black/white
        val binTile = bitmapToTile(d.bin, 640, 480, 0xFFFFFFFF.toInt(), 0xFF000000.toInt())

        // 4. All blobs: d.allBlobsMask → gray fg on black bg
        val allTile = bitmapToTile(d.allBlobsMask, 640, 480, 0xFFAAAAAA.toInt(), 0xFF202020.toInt())

        // 5. Winning blob: d.mask → green fg on black bg
        val winTile = bitmapToTile(d.mask, 640, 480, 0xFF00FF00.toInt(), 0xFF202020.toInt())

        // 6. Last event mask: same as 5 but using committed event's snapshot
        val evTile = if (lastEventMask != null)
            bitmapToTile(lastEventMask, 640, 480, 0xFF00FF00.toInt(), 0xFF202020.toInt())
        else
            IntArray(tileBytes) { 0xFF202020.toInt() }

        return Panels(rawTile, roiTile, binTile, allTile, winTile, evTile)
    }

    /**
     * Convert a packed bitmap (LSB-first, `w*h/8` bytes) into a TILE_W×TILE_H
     * ARGB tile where set bits → `fgArgb`, clear bits → `bgArgb`. Nearest-
     * neighbour downsample.
     */
    internal fun bitmapToTile(
        bits: ByteArray, w: Int, h: Int, fgArgb: Int, bgArgb: Int,
    ): IntArray {
        require(bits.size == w * h / 8) { "bits size mismatch" }
        val out = IntArray(TILE_W * TILE_H)
        for (ty in 0 until TILE_H) {
            val sy = (ty * h) / TILE_H
            for (tx in 0 until TILE_W) {
                val sx = (tx * w) / TILE_W
                val i = sy * w + sx
                val bit = (bits[i shr 3].toInt() ushr (i and 7)) and 1
                out[ty * TILE_W + tx] = if (bit == 1) fgArgb else bgArgb
            }
        }
        return out
    }
}
```

- [ ] **Step 5.2: Create `DiagnosticsRendererTest.kt`**

```kotlin
package com.tpv.bench

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticsRendererTest {

    private val MASK_BYTES = 640 * 480 / 8
    private val Y_BYTES = 640 * 480

    private fun detection() = TpvDetectionDebugV2(
        det = TpvDetection(0, 2, 320, 240, 0, 200),
        features = TpvFeatures(IntArray(7), 0, 0, 0),
        distancesSq = IntArray(5),
        bbox = TpvBbox(0, 0, 0, 0),
        areaPx = 0,
        grid8x8 = 0,
        bin = ByteArray(MASK_BYTES),
        allBlobsMask = ByteArray(MASK_BYTES),
        mask = ByteArray(MASK_BYTES),
    )

    @Test
    fun `render returns six tiles each TILE_W times TILE_H size`() {
        val rawY = ByteArray(Y_BYTES) { 128.toByte() }
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        val expected = DiagnosticsRenderer.TILE_W * DiagnosticsRenderer.TILE_H
        assertEquals(expected, panels.rawY.size)
        assertEquals(expected, panels.roiCrop.size)
        assertEquals(expected, panels.binarized.size)
        assertEquals(expected, panels.allBlobs.size)
        assertEquals(expected, panels.winningBlob.size)
        assertEquals(expected, panels.lastEventMask.size)
    }

    @Test
    fun `raw Y tile preserves luminance mid-gray`() {
        val rawY = ByteArray(Y_BYTES) { 128.toByte() }
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        // Any pixel should be ARGB = FF808080
        val expected = 0xFF808080.toInt()
        for (p in panels.rawY) assertEquals(expected, p)
    }

    @Test
    fun `binarized with all-zero bin yields all background`() {
        val rawY = ByteArray(Y_BYTES)
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        // bg color is 0xFF000000 per renderer impl
        for (p in panels.binarized) assertEquals(0xFF000000.toInt(), p)
    }

    @Test
    fun `winning blob tile is all-black when mask is empty`() {
        val rawY = ByteArray(Y_BYTES)
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        // bg color for winning blob tile is 0xFF202020 per renderer impl
        for (p in panels.winningBlob) assertEquals(0xFF202020.toInt(), p)
    }

    @Test
    fun `winning blob tile shows green when mask bit is set at 0,0`() {
        val rawY = ByteArray(Y_BYTES)
        val d = detection()
        d.mask[0] = 0x01  // pixel (0, 0) set
        val panels = DiagnosticsRenderer.render(
            rawY, d, YuvAdapter.CropRect(0, 0, 640, 480), null)
        // Tile pixel (0, 0) samples source (0, 0) → should be green
        assertEquals(0xFF00FF00.toInt(), panels.winningBlob[0])
    }

    @Test
    fun `lastEventMask null yields all dark gray tile`() {
        val rawY = ByteArray(Y_BYTES)
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        for (p in panels.lastEventMask) assertEquals(0xFF202020.toInt(), p)
    }

    @Test
    fun `ROI dim darkens pixels outside the ROI rect`() {
        val rawY = ByteArray(Y_BYTES) { 200.toByte() }
        // ROI covers only top-left 320×240; bottom-right should be dimmed
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 320, 240), null)
        // Inside ROI (tile 0,0): preserved 200
        val insideArgb = 0xFF shl 24 or (200 shl 16) or (200 shl 8) or 200
        assertEquals(insideArgb, panels.roiCrop[0])
        // Outside ROI (bottom-right corner): dimmed to 200 * 40 / 100 = 80
        val outsideArgb = 0xFF shl 24 or (80 shl 16) or (80 shl 8) or 80
        val brCorner = (DiagnosticsRenderer.TILE_H - 1) * DiagnosticsRenderer.TILE_W +
            (DiagnosticsRenderer.TILE_W - 1)
        assertEquals(outsideArgb, panels.roiCrop[brCorner])
    }
}
```

- [ ] **Step 5.3: Create `DiagnosticsView.kt` — thin Android wrapper**

```kotlin
package com.tpv.bench

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View
import java.util.concurrent.atomic.AtomicReference

/**
 * Six-cell diagnostic panel showing pipeline intermediate stages.
 * Pure rendering logic in DiagnosticsRenderer; this View just lays out
 * six Bitmaps plus labels on a Canvas.
 *
 * Thread model: camera executor calls update(); UI thread calls onDraw.
 * An AtomicReference hands off the latest Panels.
 *
 * Throttled to ~10 Hz — DiagnosticsRenderer is lightweight but 6 IntArrays
 * per frame @ 24 fps is wasteful when the human eye only perceives ~10 Hz.
 */
class DiagnosticsView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null,
) : View(context, attrs) {

    private data class State(
        val panels: DiagnosticsRenderer.Panels,
        val statusLine: String,
    )

    private val latest = AtomicReference<State?>(null)
    private val bitmaps = Array(6) {
        Bitmap.createBitmap(
            DiagnosticsRenderer.TILE_W,
            DiagnosticsRenderer.TILE_H,
            Bitmap.Config.ARGB_8888,
        )
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 20f ; color = Color.WHITE
    }
    private val bgPaint = Paint().apply { color = 0xFF101010.toInt() }

    @Volatile private var lastUpdateMs: Long = 0L
    private val throttleMs = 100L  // 10 Hz

    fun update(panels: DiagnosticsRenderer.Panels, statusLine: String) {
        val now = System.currentTimeMillis()
        if (now - lastUpdateMs < throttleMs) return
        lastUpdateMs = now
        latest.set(State(panels, statusLine))
        postInvalidate()
    }

    fun clear() { latest.set(null) ; postInvalidate() }

    override fun onDraw(canvas: Canvas) {
        val s = latest.get() ?: return
        val vw = width ; val vh = height
        if (vw == 0 || vh == 0) return
        canvas.drawRect(0f, 0f, vw.toFloat(), vh.toFloat(), bgPaint)

        // Fill the 6 prepared bitmaps with the latest IntArray pixels
        val p = s.panels
        bitmaps[0].setPixels(p.rawY,        0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[1].setPixels(p.roiCrop,     0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[2].setPixels(p.binarized,   0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[3].setPixels(p.allBlobs,    0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[4].setPixels(p.winningBlob, 0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[5].setPixels(p.lastEventMask,0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)

        val labels = arrayOf("raw Y", "ROI", "bin", "all blobs", "winner", "last event")

        // 2 rows × 3 cols layout
        val cellW = vw / 3
        val cellH = vh / 2
        val padX = 4
        val padY = 4
        val labelH = 24
        for (cell in 0 until 6) {
            val row = cell / 3 ; val col = cell % 3
            val x0 = col * cellW + padX
            val y0 = row * cellH + padY
            val bmpW = cellW - 2 * padX
            val bmpH = cellH - 2 * padY - labelH
            val dst = android.graphics.RectF(
                x0.toFloat(), y0.toFloat(),
                (x0 + bmpW).toFloat(), (y0 + bmpH).toFloat(),
            )
            canvas.drawBitmap(bitmaps[cell], null, dst, null)
            canvas.drawText(labels[cell], x0.toFloat(), (y0 + bmpH + labelH - 4).toFloat(), labelPaint)
        }
    }
}
```

- [ ] **Step 5.4: Run unit tests**

Run: `cd android && ./gradlew :app:testDebugUnitTest 2>&1 | tail -5`

Expected: 31 (from T3) + 4 (T4 OverlayPainter new) + 7 (DiagnosticsRenderer new) = 42 tests pass.

Wait — T4 added 4 tests to OverlayPainterTest (9→13), and T5 adds DiagnosticsRendererTest (7). So expected total = 31 + 4 + 7 = 42. Confirm with `./gradlew :app:testDebugUnitTest 2>&1 | grep -c passed` or read XMLs.

- [ ] **Step 5.5: Build APK**

Run: `make android-apk 2>&1 | tail -5`

Expected: `BUILD SUCCESSFUL`. DiagnosticsView is declared but not yet used in `activity_main.xml` (Task 6 wires it in).

- [ ] **Step 5.6: Commit**

```bash
git add android/app/src/main/java/com/tpv/bench/DiagnosticsRenderer.kt \
        android/app/src/main/java/com/tpv/bench/DiagnosticsView.kt \
        android/app/src/test/java/com/tpv/bench/DiagnosticsRendererTest.kt
git commit -m "feat(android): DiagnosticsRenderer (pure logic, 7 tests) + DiagnosticsView

Six 160×120 ARGB tiles: raw Y / ROI dim / binarized / all CCL blobs /
winning blob mask / last committed event mask. Nearest-neighbour
downsample from 640×480. Pure-Kotlin renderer is JVM-testable; View
is a thin Canvas wrapper that setPixels() into six pre-allocated
Bitmaps and lays them out 2×3 with labels.

Update throttled to 10 Hz (human eye limit; saves CPU during high FPS).
View not yet wired into activity_main.xml — MainActivity integration
in T6."
```

---

## Task 6: MainActivity + SettingsState + XML — v2 integration

**Goal:** Settings with 3 tabs (Trigger / Pipeline / ROI). Everything run-locked. New buttons `Diag / ROI / Clear`. Status line `size:N [w×h] grid:M rotation:θ°` above preview. `onFrame` switches to `processFrameDebugV2` using snapshot params. `renderOverlayJpeg` uses mask. `RunRecorder.recordEvent` passes the mask through.

**Files:**
- Modify: `android/app/src/main/java/com/tpv/bench/SettingsState.kt`
- Modify: `android/app/src/main/res/layout/activity_main.xml`
- Modify: `android/app/src/main/java/com/tpv/bench/MainActivity.kt`

---

- [ ] **Step 6.1: Extend `SettingsState.kt` with v2 fields**

Append after existing `mDriftPx`:

```kotlin
    var binThreshold: Int
        get() = prefs.getInt("bin_threshold", 128).coerceIn(0, 255)
        set(v) = prefs.edit { putInt("bin_threshold", v.coerceIn(0, 255)) }

    var darkObjectMode: Boolean
        get() = prefs.getBoolean("dark_object_mode", true)   // default TRUE per spec §5.5
        set(v) = prefs.edit { putBoolean("dark_object_mode", v) }

    var roiX: Int
        get() = prefs.getInt("roi_x", 0).coerceIn(0, 639)
        set(v) = prefs.edit { putInt("roi_x", v.coerceIn(0, 639)) }

    var roiY: Int
        get() = prefs.getInt("roi_y", 0).coerceIn(0, 479)
        set(v) = prefs.edit { putInt("roi_y", v.coerceIn(0, 479)) }

    var roiW: Int
        get() = prefs.getInt("roi_w", 640).coerceIn(1, 640)
        set(v) = prefs.edit { putInt("roi_w", v.coerceIn(1, 640)) }

    var roiH: Int
        get() = prefs.getInt("roi_h", 480).coerceIn(1, 480)
        set(v) = prefs.edit { putInt("roi_h", v.coerceIn(1, 480)) }
```

- [ ] **Step 6.2: Rewrite `activity_main.xml`** with status line + new buttons

```xml
<?xml version="1.0" encoding="utf-8"?>
<androidx.constraintlayout.widget.ConstraintLayout
    xmlns:android="http://schemas.android.com/apk/res-auto"
    xmlns:app="http://schemas.android.com/apk/res-auto"
    android:layout_width="match_parent"
    android:layout_height="match_parent">

    <LinearLayout
        android:id="@+id/topbar"
        android:layout_width="match_parent"
        android:layout_height="wrap_content"
        android:orientation="horizontal"
        android:padding="8dp"
        app:layout_constraintTop_toTopOf="parent">
        <Button android:id="@+id/btn_start"   android:layout_width="wrap_content" android:layout_height="wrap_content" android:text="@string/btn_start" />
        <Button android:id="@+id/btn_stop"    android:layout_width="wrap_content" android:layout_height="wrap_content" android:text="@string/btn_stop" android:enabled="false" />
        <Button android:id="@+id/btn_export"  android:layout_width="wrap_content" android:layout_height="wrap_content" android:text="@string/btn_export" android:enabled="false" />
        <Button android:id="@+id/btn_settings" android:layout_width="wrap_content" android:layout_height="wrap_content" android:text="@string/btn_settings" />
        <Button android:id="@+id/btn_diag"    android:layout_width="wrap_content" android:layout_height="wrap_content" android:text="Diag" />
        <Button android:id="@+id/btn_roi"     android:layout_width="wrap_content" android:layout_height="wrap_content" android:text="ROI" />
        <Button android:id="@+id/btn_clear"   android:layout_width="wrap_content" android:layout_height="wrap_content" android:text="Clear" />
    </LinearLayout>

    <TextView
        android:id="@+id/status_line"
        android:layout_width="match_parent"
        android:layout_height="wrap_content"
        android:padding="6dp"
        android:background="#80000000"
        android:textColor="#FFFFFF"
        android:fontFamily="monospace"
        android:textSize="12sp"
        android:text="size:- [-×-] grid:- rotation:-°      FPS:0.0 skipped:0"
        app:layout_constraintTop_toBottomOf="@id/topbar" />

    <androidx.camera.view.PreviewView
        android:id="@+id/preview"
        android:layout_width="0dp"
        android:layout_height="0dp"
        app:layout_constraintTop_toBottomOf="@id/status_line"
        app:layout_constraintBottom_toTopOf="@id/diag"
        app:layout_constraintStart_toStartOf="parent"
        app:layout_constraintEnd_toEndOf="parent" />

    <com.tpv.bench.OverlayView
        android:id="@+id/overlay"
        android:layout_width="0dp"
        android:layout_height="0dp"
        app:layout_constraintTop_toTopOf="@id/preview"
        app:layout_constraintBottom_toBottomOf="@id/preview"
        app:layout_constraintStart_toStartOf="@id/preview"
        app:layout_constraintEnd_toEndOf="@id/preview" />

    <com.tpv.bench.DiagnosticsView
        android:id="@+id/diag"
        android:layout_width="match_parent"
        android:layout_height="0dp"
        android:visibility="gone"
        app:layout_constraintBottom_toTopOf="@id/hud"
        app:layout_constraintHeight_percent="0.35" />

    <TextView
        android:id="@+id/hud"
        android:layout_width="match_parent"
        android:layout_height="wrap_content"
        android:padding="6dp"
        android:background="#80000000"
        android:textColor="#FFFFFF"
        android:textSize="12sp"
        android:fontFamily="monospace"
        android:text="State: IDLE    Events: 0"
        app:layout_constraintBottom_toBottomOf="parent" />
</androidx.constraintlayout.widget.ConstraintLayout>
```

- [ ] **Step 6.3: `MainActivity.kt` — fields, snapshot, processFrameDebugV2 call, status line, new buttons**

This is the largest single-file change in the plan. Replace the whole class body as follows (keep the `package` line, imports, top-level data class `SettingsSnapshot` update):

Add these imports:

```kotlin
import android.text.InputType
import android.widget.CheckBox
```

Extend `SettingsSnapshot` private data class:

```kotlin
private data class SettingsSnapshot(
    val n: Int, val k: Int, val m: Int,
    val binThreshold: Int,
    val darkObjectMode: Boolean,
    val roiX: Int, val roiY: Int, val roiW: Int, val roiH: Int,
)
```

Add new fields in `MainActivity`:

```kotlin
    private lateinit var statusLine: TextView
    private lateinit var diagView: DiagnosticsView
    private lateinit var btnDiag: Button
    private lateinit var btnRoi: Button
    private lateinit var btnClear: Button

    @Volatile private var lastRawY: ByteArray? = null
    @Volatile private var lastRoi: YuvAdapter.CropRect = YuvAdapter.CropRect(0, 0, 640, 480)
    @Volatile private var lastEventMask: ByteArray? = null
    @Volatile private var activeSnapshot: SettingsSnapshot? = null
```

Update `onCreate`:

```kotlin
override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_main)

    settings = SettingsState(this)
    camera = CameraAdapter(this)
    yuv = YuvAdapter(640, 480)
    preview = findViewById(R.id.preview)
    overlay = findViewById(R.id.overlay)
    hud = findViewById(R.id.hud)
    statusLine = findViewById(R.id.status_line)
    diagView = findViewById(R.id.diag)
    btnStart = findViewById(R.id.btn_start)
    btnStop = findViewById(R.id.btn_stop)
    btnExport = findViewById(R.id.btn_export)
    btnSettings = findViewById(R.id.btn_settings)
    btnDiag = findViewById(R.id.btn_diag)
    btnRoi = findViewById(R.id.btn_roi)
    btnClear = findViewById(R.id.btn_clear)

    btnStart.setOnClickListener { requestCameraAndStart() }
    btnStop.setOnClickListener { onStopClicked() }
    btnExport.setOnClickListener { onExportClicked() }
    btnSettings.setOnClickListener { showSettingsDialog() }
    btnDiag.setOnClickListener { toggleDiagView() }
    btnRoi.setOnClickListener { showSettingsDialog(focusTab = "roi") }
    btnClear.setOnClickListener { onClearClicked() }
}

private fun toggleDiagView() {
    diagView.visibility = if (diagView.visibility == View.VISIBLE) View.GONE else View.VISIBLE
}

private fun onClearClicked() {
    lastCommittedEvent.set(null)
    lastEventMask = null
    overlay.reset()
    diagView.clear()
    updateHud(null, FramePresence.EMPTY)   // reset HUD Last block
}
```

Update `onStartClicked` to snapshot v2 settings:

```kotlin
private fun onStartClicked() {
    if (running.get()) return
    val freeBytes = filesDir.usableSpace
    if (freeBytes < 1_000_000_000L) {
        toast("Less than 1 GB free; consider cleaning old runs")
    }

    val sdf = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US)
        .apply { timeZone = TimeZone.getTimeZone("UTC") }
    val runId = "run_${sdf.format(Date())}"
    val runDir = File(filesDir, "runs/$runId").apply { mkdirs() }

    // Snapshot ALL v2 settings as a run-level constant
    val snap = SettingsSnapshot(
        n = settings.nStable, k = settings.kEmpty, m = settings.mDriftPx,
        binThreshold = settings.binThreshold,
        darkObjectMode = settings.darkObjectMode,
        roiX = settings.roiX, roiY = settings.roiY,
        roiW = settings.roiW, roiH = settings.roiH,
    )
    activeSnapshot = snap
    lastRoi = YuvAdapter.CropRect(snap.roiX, snap.roiY, snap.roiW, snap.roiH)

    // Pre-compute SHA-256 values on the UI thread (before first frame)
    soSha256 = sha256Of(File(applicationInfo.nativeLibraryDir, "libtpv.so").readBytes())
    modelSha256 = try {
        assets.open("tpv_model_sha.txt").bufferedReader().readText().trim()
    } catch (e: Exception) { "unknown" }

    pendingInit.set { nativeW, nativeH, crop ->
        val meta = buildMeta(runId, snap, nativeW, nativeH, crop)
        recorder = RunRecorder(runDir, meta).also { it.start() }
    }
    trigger = TriggerMachine(snap.n, snap.k, snap.m)
    frameCounter.set(0) ; eventCounter.set(0) ; fpsWin.clear()
    skippedCount.set(0) ; lastArrivalNs = 0L ; recentGaps.clear()
    lastCommittedEvent.set(null) ; lastEventMask = null
    overlay.reset() ; diagView.clear()

    running.set(true)
    lockUi(true)
    camera.start(this, preview) { proxy -> onFrame(proxy) }
    Log.i(TAG, "Started run $runId (v2, dark_mode=${snap.darkObjectMode}, roi=${snap.roiX},${snap.roiY},${snap.roiW},${snap.roiH})")
}

private fun lockUi(runActive: Boolean) = runOnUiThread {
    btnStart.isEnabled = !runActive
    btnStop.isEnabled = runActive
    btnSettings.isEnabled = !runActive
    btnRoi.isEnabled = !runActive
    // Diag / Clear / Export: always enabled
}
```

Rewrite `onFrame`:

```kotlin
private fun onFrame(proxy: androidx.camera.core.ImageProxy) {
    val tCamArrive = System.nanoTime()
    try {
        if (lastArrivalNs != 0L) {
            val gap = tCamArrive - lastArrivalNs
            recentGaps.addLast(gap)
            while (recentGaps.size > 30) recentGaps.removeFirst()
            if (recentGaps.size >= 5) {
                val sorted = recentGaps.toLongArray().sortedArray()
                val median = sorted[sorted.size / 2]
                if (median > 0 && gap > median * SKIP_GAP_RATIO_NUM / SKIP_GAP_RATIO_DEN) {
                    val missed = (gap + median / 2) / median - 1
                    if (missed > 0) skippedCount.addAndGet(missed)
                }
            }
        }
        lastArrivalNs = tCamArrive

        val nativeW = proxy.width ; val nativeH = proxy.height
        val yPlane = proxy.planes[0]
        val yBuf = yPlane.buffer
        val yArr = ByteArray(yBuf.remaining()).also { yBuf.get(it) }
        val adapted = yuv.extract(yArr, yPlane.rowStride, nativeW, nativeH)
        val nv21 = Yuv420ToNv21.convert(proxy)
        lastRawY = adapted.y

        pendingInit.getAndSet(null)?.invoke(nativeW, nativeH, adapted.crop)

        val frameIdx = frameCounter.incrementAndGet().toLong()
        val snap = activeSnapshot ?: return  // finally still closes proxy

        val timingBuf = LongArray(3)
        val result = TpvNative.processFrameDebugV2(
            adapted.y, 640, 480,
            snap.binThreshold, snap.darkObjectMode,
            snap.roiX, snap.roiY, snap.roiW, snap.roiH,
            timingBuf,
        )
        val tJniReturn = System.nanoTime()

        val presence = when {
            result.det.status == 0 -> FramePresence.PRESENT
            result.det.status == 1 -> FramePresence.EMPTY
            else -> FramePresence.DROP
        }
        val timingClassId = if (presence == FramePresence.DROP) -1 else result.det.classId

        recorder?.recordFrameTiming(FrameTiming(
            frameIdxInRun = frameIdx,
            tpvStatus = result.det.status, tpvClassId = timingClassId,
            tCameraArriveNs = tCamArrive,
            tJniEnterNs  = timingBuf[0],
            tTpvEnterNs  = timingBuf[1],
            tTpvExitNs   = timingBuf[2],
            tJniReturnNs = tJniReturn,
        ))

        val obs = FrameObservation(
            presence = presence,
            x = if (presence == FramePresence.PRESENT) result.det.x else 0,
            y = if (presence == FramePresence.PRESENT) result.det.y else 0,
            classId = if (presence == FramePresence.PRESENT) result.det.classId else -1,
            frameIdxInRun = frameIdx,
            detection = if (presence == FramePresence.PRESENT) result else null,
        )
        val out = trigger?.onFrame(obs) ?: StateMachineOutput.None

        if (out is StateMachineOutput.Commit) {
            val triggerTsMs = System.currentTimeMillis()
            val jpg = renderOverlayJpeg(
                out.event.triggerFrameDebug, adapted.crop, nativeW, nativeH,
                out.event.eventClassId, out.event.flicker, nv21,
            )
            recorder?.recordEvent(
                out.event, triggerTsMs = triggerTsMs,
                rawY = adapted.y, overlayJpeg = jpg,
                mask = out.event.triggerFrameDebug.mask,
            )
            eventCounter.incrementAndGet()
            lastCommittedEvent.set(out.event)
            lastEventMask = out.event.triggerFrameDebug.mask
            overlay.onCommit(out.event.eventClassId, out.event.flicker)
        }

        if (presence == FramePresence.PRESENT) {
            overlay.updateLive(result, lastRoi, adapted.crop, nativeW, nativeH)
        } else {
            overlay.clearLive()
        }

        // Update DiagnosticsView (it internally throttles to 10 Hz)
        if (diagView.visibility == View.VISIBLE) {
            val panels = DiagnosticsRenderer.render(
                adapted.y, result, lastRoi, lastEventMask,
            )
            diagView.update(panels, "")
        }

        updateHud(result, presence)
    } finally {
        proxy.close()
    }
}
```

Replace `updateHud` to render the status line as well:

```kotlin
private fun updateHud(live: TpvDetectionDebugV2?, presence: FramePresence) {
    val now = System.nanoTime()
    fpsWin.addLast(now)
    while (fpsWin.size > fpsWindowSize) fpsWin.removeFirst()
    val fps = if (fpsWin.size >= 2) {
        (fpsWin.size - 1).toDouble() * 1_000_000_000.0 / (fpsWin.last() - fpsWin.first())
    } else 0.0

    val st = trigger?.state ?: MachineState.IDLE

    // Status line — live per-frame
    val statusText = if (live != null && presence == FramePresence.PRESENT) {
        val theta = live.det.thetaX10 / 10.0
        "size:${live.areaPx} [${live.bbox.w}×${live.bbox.h}] grid:${live.grid8x8} rotation:%.1f°    FPS:%.1f skipped:%d"
            .format(theta, fps, skippedCount.get())
    } else {
        "size:- [-×-] grid:- rotation:-°    FPS:%.1f skipped:%d"
            .format(fps, skippedCount.get())
    }
    runOnUiThread { statusLine.text = statusText }

    // HUD — State + Events + Last event info
    val ev = lastCommittedEvent.get()
    val hudMsg = buildString {
        append("State: $st    Events: ${eventCounter.get()}\n")
        if (ev != null) {
            val d = ev.triggerFrameDebug
            append("Last event#${ev.eventIdx}: cls=${ev.eventClassId} flicker=${ev.flicker} ")
            append("size=${d.areaPx} θ=${d.det.thetaX10 / 10.0}°")
        } else {
            append("Last (no committed event yet)")
        }
    }
    runOnUiThread { hud.text = hudMsg }
}
```

Update `buildMeta` signature + body:

```kotlin
private fun buildMeta(
    runId: String, s: SettingsSnapshot,
    nativeW: Int, nativeH: Int, crop: YuvAdapter.CropRect,
): MetaInfo {
    return MetaInfo(
        runId = runId,
        deviceModel = Build.MODEL, androidLevel = Build.VERSION.SDK_INT,
        abi = Build.SUPPORTED_ABIS.firstOrNull() ?: "unknown",
        cpuMaxFreqKhz = readMaxCpuFreqKhz(),
        soSha256 = soSha256, modelDataSha256 = modelSha256,
        nClasses = TpvNative.nClasses(), binThreshold = s.binThreshold,
        darkObjectMode = s.darkObjectMode,
        roiX = s.roiX, roiY = s.roiY, roiW = s.roiW, roiH = s.roiH,
        nStable = s.n, kEmpty = s.k, mDriftPx = s.m,
        requestedW = 640, requestedH = 480,
        nativeW = nativeW, nativeH = nativeH,
        cropX = crop.x, cropY = crop.y, cropW = crop.w, cropH = crop.h,
        downsampleRatioX = crop.w / 640.0, downsampleRatioY = crop.h / 480.0,
    )
}
```

Add the v2 3-tab settings dialog. Replace `showSettingsDialog()` with:

```kotlin
private fun showSettingsDialog(focusTab: String = "trigger") {
    if (running.get()) { toast("Stop the run before changing settings") ; return }

    val container = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL ; setPadding(32, 16, 32, 16)
    }

    fun sectionHeader(title: String) = TextView(this).apply {
        text = title ; textSize = 16f ; setPadding(0, 16, 0, 8)
    }

    container.addView(sectionHeader("Trigger"))
    val nEt = EditText(this).apply { setText(settings.nStable.toString()) ; hint = "N_stable" ; inputType = InputType.TYPE_CLASS_NUMBER }
    val kEt = EditText(this).apply { setText(settings.kEmpty.toString()) ; hint = "K_empty" ; inputType = InputType.TYPE_CLASS_NUMBER }
    val mEt = EditText(this).apply { setText(settings.mDriftPx.toString()) ; hint = "M_drift_px" ; inputType = InputType.TYPE_CLASS_NUMBER }
    container.addView(nEt) ; container.addView(kEt) ; container.addView(mEt)

    container.addView(sectionHeader("Pipeline"))
    val darkCb = CheckBox(this).apply { text = "Dark object mode (Y < threshold = fg)" ; isChecked = settings.darkObjectMode }
    val thrEt = EditText(this).apply { setText(settings.binThreshold.toString()) ; hint = "bin_threshold (0-255)" ; inputType = InputType.TYPE_CLASS_NUMBER }
    container.addView(darkCb) ; container.addView(thrEt)

    container.addView(sectionHeader("ROI"))
    val roiXEt = EditText(this).apply { setText(settings.roiX.toString()) ; hint = "x (0-639)" ; inputType = InputType.TYPE_CLASS_NUMBER }
    val roiYEt = EditText(this).apply { setText(settings.roiY.toString()) ; hint = "y (0-479)" ; inputType = InputType.TYPE_CLASS_NUMBER }
    val roiWEt = EditText(this).apply { setText(settings.roiW.toString()) ; hint = "w (1-640)" ; inputType = InputType.TYPE_CLASS_NUMBER }
    val roiHEt = EditText(this).apply { setText(settings.roiH.toString()) ; hint = "h (1-480)" ; inputType = InputType.TYPE_CLASS_NUMBER }
    container.addView(roiXEt) ; container.addView(roiYEt) ; container.addView(roiWEt) ; container.addView(roiHEt)

    AlertDialog.Builder(this)
        .setTitle("Settings (run-locked)")
        .setView(container)
        .setPositiveButton("OK") { _, _ ->
            settings.nStable = nEt.text.toString().toIntOrNull() ?: 3
            settings.kEmpty  = kEt.text.toString().toIntOrNull() ?: 5
            settings.mDriftPx = mEt.text.toString().toIntOrNull() ?: 30
            settings.darkObjectMode = darkCb.isChecked
            settings.binThreshold   = thrEt.text.toString().toIntOrNull() ?: 128
            settings.roiX = roiXEt.text.toString().toIntOrNull() ?: 0
            settings.roiY = roiYEt.text.toString().toIntOrNull() ?: 0
            settings.roiW = roiWEt.text.toString().toIntOrNull() ?: 640
            settings.roiH = roiHEt.text.toString().toIntOrNull() ?: 480
            toast("Settings saved")
        }
        .setNegativeButton("Cancel", null)
        .show()
}
```

The `focusTab` parameter is a hook for the `ROI` button click; for now we accept all three sections live together in one dialog and ignore `focusTab`. Real tab UI is defer-able polish (out of scope for T6).

Update `renderOverlayJpeg` to also paint the mask fill (kept consistent with on-screen overlay). Add after `val bmp = BitmapFactory...`:

```kotlin
// Paint green mask fill on top of the camera image
val maskPixels = OverlayPainter.decodeMaskToArgb(
    d.mask, 640, 480, OverlayPainter.GREEN_MASK_ARGB)
val maskBmp = Bitmap.createBitmap(maskPixels, 640, 480, Bitmap.Config.ARGB_8888)
canvas.drawBitmap(maskBmp, null,
    android.graphics.Rect(crop.x, crop.y, crop.x + crop.w, crop.y + crop.h), null)

// Paint yellow ROI rect
val roiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    style = Paint.Style.STROKE ; strokeWidth = 4f
    this.color = OverlayPainter.YELLOW_ROI_ARGB
}
val (rnx0, rny0) = OverlayPainter.mapCoord(lastRoi.x, lastRoi.y, crop)
val (rnx1, rny1) = OverlayPainter.mapCoord(
    lastRoi.x + lastRoi.w, lastRoi.y + lastRoi.h, crop)
canvas.drawRect(rnx0.toFloat(), rny0.toFloat(), rnx1.toFloat(), rny1.toFloat(), roiPaint)
```

(The v1 circle+axis still runs; we want them on the JPG for consistency with on-screen overlay. Remove the old v1 `drawCircle` call — OverlayView v2 uses a red dot; the .jpg should too. Check & remove.)

The call to `renderOverlayJpeg` in `onFrame` already passes `d = out.event.triggerFrameDebug` which is now `TpvDetectionDebugV2`; its `.mask` field is accessible. Adjust method signature:

```kotlin
private fun renderOverlayJpeg(
    d: TpvDetectionDebugV2,
    crop: YuvAdapter.CropRect,
    nativeW: Int, nativeH: Int,
    eventClassId: Int, flicker: Boolean, nv21: ByteArray,
): ByteArray { /* ... */ }
```

- [ ] **Step 6.4: Build APK**

Run: `make android-apk 2>&1 | tail -5`

Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 6.5: Run unit tests (no new tests in T6; sanity check)**

Run: `cd android && ./gradlew :app:testDebugUnitTest 2>&1 | tail -5`

Expected: 42/42 still green (T3+T4+T5 added tests; T6 adds none).

- [ ] **Step 6.6: Commit**

```bash
git add android/app/src/main/java/com/tpv/bench/SettingsState.kt \
        android/app/src/main/res/layout/activity_main.xml \
        android/app/src/main/java/com/tpv/bench/MainActivity.kt
git commit -m "feat(android): MainActivity v2 — wire processFrameDebugV2 + mask + buttons

SettingsState gains binThreshold (default 128), darkObjectMode
(default true per spec §5.5), roi{X,Y,W,H} (default full-frame).
All run-locked: SettingsSnapshot captures them at Start; in-run
dialog checks running.get() and bails. One dialog with three
sections (Trigger / Pipeline / ROI).

activity_main.xml adds status_line TextView, diag view slot
(35% height, visibility=gone by default), and Diag/ROI/Clear
buttons in the top row.

MainActivity.onFrame now calls processFrameDebugV2 with snapshot
params, threads the mask through recordEvent, updates DiagnosticsView
when visible, and the status line shows live size:N [w×h] grid:M
rotation:θ° per frame. renderOverlayJpeg paints the green mask +
yellow ROI onto the saved per-event .jpg, matching the on-screen
overlay semantics."
```

---

## Task 7: `build/replay` v1/v2 mode + `tools/visualize_mask.py`

**Goal:** `build/replay` auto-selects v1 or v2 mode by reading `meta.json.ui_version`; v2 mode uses the meta's `tpv.bin_threshold / dark_object_mode / roi` as algorithm inputs. CLI flags `--v1 / --v2 / --bin-threshold / --dark-object-mode / --roi` override. CSV output gains `bbox_area_px / grid_8x8` columns. New `tools/visualize_mask.py` turns a `.mask` file into a PNG.

**Files:**
- Modify: `tools/replay.c`
- Create: `tools/visualize_mask.py`
- Modify: `Makefile` (if replay rule needs `-DTPV_DEBUG_FEATURES` — it does, for v2 entry point)

---

- [ ] **Step 7.1: Read meta.json in replay — minimal parser**

Since we're in C without a JSON library, the simplest approach is a hand-rolled parser that only extracts the 4 fields we need: `ui_version`, `tpv.bin_threshold`, `tpv.dark_object_mode`, `tpv.roi.{x,y,w,h}`. These are all integers, booleans, or simple strings at predictable positions.

Append to `tools/replay.c`:

```c
typedef struct {
    int is_v2;               /* 1 if meta declares ui_version == "v2" */
    uint8_t bin_threshold;   /* from meta.json or default 128 */
    int dark_object_mode;    /* 0 or 1 */
    int roi_x, roi_y, roi_w, roi_h;
} ReplayMeta;

/* Best-effort field lookup: finds the first occurrence of "key" : <value>
 * and parses. Not robust against escaped strings, but our meta.json is
 * produced by JSONObject.toString(2) with predictable formatting. */
static int meta_find_int(const char *json, const char *key, int *out) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p && (*p == ' ' || *p == '\n' || *p == '\t')) p++;
    if (*p == 't') { *out = 1; return 1; }  /* true */
    if (*p == 'f') { *out = 0; return 1; }  /* false */
    *out = atoi(p);
    return 1;
}

static int meta_find_string(const char *json, const char *key,
                             char *out, size_t out_size) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p && (*p == ' ' || *p == '\n' || *p == '\t')) p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size) out[n++] = *p++;
    out[n] = '\0';
    return 1;
}

static int read_meta(const char *run_dir, ReplayMeta *m) {
    /* Defaults — v1 mode / full frame / default threshold */
    m->is_v2 = 0;
    m->bin_threshold = 128;
    m->dark_object_mode = 0;
    m->roi_x = 0; m->roi_y = 0; m->roi_w = 640; m->roi_h = 480;

    char path[1024];
    snprintf(path, sizeof path, "%s/meta.json", run_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;   /* no meta.json → v1 mode with defaults */
    char buf[16384] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    char ver[32] = {0};
    if (meta_find_string(buf, "ui_version", ver, sizeof ver) &&
        strcmp(ver, "v2") == 0) m->is_v2 = 1;

    int tmp;
    if (m->is_v2) {
        if (meta_find_int(buf, "bin_threshold", &tmp)) m->bin_threshold = (uint8_t)tmp;
        if (meta_find_int(buf, "dark_object_mode", &tmp)) m->dark_object_mode = tmp;
        if (meta_find_int(buf, "x", &tmp)) m->roi_x = tmp;
        if (meta_find_int(buf, "y", &tmp)) m->roi_y = tmp;
        if (meta_find_int(buf, "w", &tmp)) m->roi_w = tmp;
        if (meta_find_int(buf, "h", &tmp)) m->roi_h = tmp;
    }
    return 1;
}
```

Note: `meta_find_int(..., "x", ...)` will match the first `"x"` in the JSON — we know from our meta.json structure that the first occurrence of `"x"` is inside `tpv.roi.{x,...}` only if the producer writes tpv before camera. Check the actual output; if order isn't guaranteed, bracket-nest the parser. For simplicity, require the APP's `metaToJson()` to emit `tpv` before `camera` — already the case in T3 Step 3.5 (order is `device` → `tpv` → `trigger` → `camera`).

- [ ] **Step 7.2: Rewire `main()` in replay.c with v1/v2 mode selection + CLI flags**

Replace the existing main loop to use the metadata and call `tpv_process_frame` (v1) or `tpv_process_frame_debug_v2` (v2):

```c
#define TPV_DEBUG_FEATURES 1  /* ensure v2 function is linkable */
#include "tpv_internal.h"

static int parse_roi(const char *s, int *x, int *y, int *w, int *h) {
    return sscanf(s, "%d,%d,%d,%d", x, y, w, h) == 4;
}

int main(int argc, char **argv) {
    const char *run_dir = NULL;
    int force_v1 = 0, force_v2 = 0;
    int override_thr = -1, override_dark = -1;
    int override_roi_x = -1, roi_y, roi_w, roi_h;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--v1")) force_v1 = 1;
        else if (!strcmp(argv[i], "--v2")) force_v2 = 1;
        else if (!strcmp(argv[i], "--bin-threshold") && i+1 < argc) {
            override_thr = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dark-object-mode") && i+1 < argc) {
            override_dark = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--roi") && i+1 < argc) {
            if (!parse_roi(argv[++i], &override_roi_x, &roi_y, &roi_w, &roi_h)) {
                fprintf(stderr, "--roi needs x,y,w,h\n"); return 2;
            }
        } else if (argv[i][0] != '-') {
            run_dir = argv[i];
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2;
        }
    }
    if (!run_dir) {
        fprintf(stderr, "usage: replay [--v1|--v2] [--bin-threshold N] [--dark-object-mode 0|1] [--roi x,y,w,h] <run_dir_or_frames_dir>\n");
        return 2;
    }

    ReplayMeta meta;
    read_meta(run_dir, &meta);
    if (force_v1) meta.is_v2 = 0;
    if (force_v2) meta.is_v2 = 1;
    if (override_thr >= 0)  meta.bin_threshold = (uint8_t)override_thr;
    if (override_dark >= 0) meta.dark_object_mode = override_dark;
    if (override_roi_x >= 0) {
        meta.roi_x = override_roi_x; meta.roi_y = roi_y;
        meta.roi_w = roi_w; meta.roi_h = roi_h;
    }

    /* ... existing slurp_sorted + directory scan ... */
    printf("frame_name,status,class_id,x,y,theta_x10,confidence,bbox_area_px,grid_8x8\n");
    static uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    for (size_t i = 0; i < nn; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", run_dir, names[i]);
        /* Only process files ending in .y */
        size_t nlen = strlen(names[i]);
        if (nlen < 2 || strcmp(names[i] + nlen - 2, ".y") != 0) { free(names[i]); continue; }
        FILE *f = fopen(path, "rb");
        if (!f) { free(names[i]); continue; }
        if (fread(y, 1, sizeof y, f) == sizeof y) {
            if (meta.is_v2) {
                tpv_DetectionDebugV2 d;
                int rc = tpv_process_frame_debug_v2(
                    y, TPV_WIDTH, TPV_HEIGHT,
                    meta.bin_threshold, meta.dark_object_mode,
                    meta.roi_x, meta.roi_y, meta.roi_w, meta.roi_h,
                    &d);
                printf("%s,%d,%u,%d,%d,%d,%u,%d,%d\n",
                    names[i], rc, d.det.class_id, d.det.x, d.det.y,
                    d.det.theta_x10, d.det.confidence_q8,
                    d.area_px, d.grid_8x8);
            } else {
                tpv_Detection dv1;
                int rc = tpv_process_frame(y, TPV_WIDTH, TPV_HEIGHT, &dv1);
                printf("%s,%d,%u,%d,%d,%d,%u,,\n",
                    names[i], rc, dv1.class_id, dv1.x, dv1.y,
                    dv1.theta_x10, dv1.confidence_q8);
            }
        }
        fclose(f);
        free(names[i]);
    }
    free(names);
    return 0;
}
```

Adjust the existing directory-scan logic (`nn`, `names[]`) to only consider `*.y` — don't skip other files entirely but emit CSV rows only for `.y`.

- [ ] **Step 7.3: Rebuild replay with `-DTPV_DEBUG_FEATURES`**

Locate the `build/replay` rule in the root `Makefile` (added in v1 T9 or earlier). Change:

```make
build/replay: tools/replay.c $(SRCS) src/model_data.c | build
	$(CC_HOST) $(CFLAGS_HOST) -o $@ $^ -lm
```

to:

```make
build/replay: tools/replay.c $(SRCS) src/model_data.c | build
	$(CC_HOST) $(CFLAGS_HOST) -DTPV_DEBUG_FEATURES -o $@ $^ -lm
```

- [ ] **Step 7.4: Create `tools/visualize_mask.py`**

```python
#!/usr/bin/env python3
"""
Render a .mask file (LSB-first packed bitmap, 640*480/8 = 38400 bytes)
into a PNG where set bits are white and clear bits are black.

Usage: python3 tools/visualize_mask.py <mask_file> [-o out.png]
Default output: same path with .mask → .png

No external dependencies beyond Pillow (standard install).
"""
import os
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("install Pillow:  pip install pillow")

WIDTH = 640
HEIGHT = 480
EXPECTED = WIDTH * HEIGHT // 8

def decode_mask(buf: bytes) -> bytes:
    if len(buf) != EXPECTED:
        raise ValueError(f"mask size {len(buf)} != {EXPECTED}")
    out = bytearray(WIDTH * HEIGHT)
    for i in range(WIDTH * HEIGHT):
        byte = buf[i >> 3]
        bit = (byte >> (i & 7)) & 1
        out[i] = 255 if bit else 0
    return bytes(out)

def main():
    if len(sys.argv) < 2:
        sys.exit("usage: visualize_mask.py <mask_file> [-o out.png]")
    inp = sys.argv[1]
    out = None
    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == "-o" and i + 1 < len(sys.argv):
            out = sys.argv[i+1] ; i += 2
        else:
            sys.exit(f"unknown arg: {sys.argv[i]}")
    if out is None:
        out = str(Path(inp).with_suffix(".png"))

    with open(inp, "rb") as f:
        raw = f.read()
    pixels = decode_mask(raw)
    img = Image.frombytes("L", (WIDTH, HEIGHT), pixels)
    img.save(out)
    n_fg = sum(1 for b in pixels if b == 255)
    print(f"{inp} → {out}  ({n_fg} foreground px, {n_fg * 100.0 / (WIDTH * HEIGHT):.2f}%)")

if __name__ == "__main__":
    main()
```

Then `chmod +x tools/visualize_mask.py`.

- [ ] **Step 7.5: Rebuild replay + verify**

Run: `make clean && make build/replay 2>&1 | tail -3`

Expected: clean compile, `build/replay` binary present.

- [ ] **Step 7.6: Sanity-check visualize_mask.py with an all-zero .mask**

```bash
dd if=/dev/zero of=/tmp/zero.mask bs=38400 count=1 2>/dev/null
python3 tools/visualize_mask.py /tmp/zero.mask -o /tmp/zero.png
```

Expected output:
```
/tmp/zero.mask → /tmp/zero.png  (0 foreground px, 0.00%)
```

And `file /tmp/zero.png` should report `PNG image data, 640 x 480, 8-bit grayscale`.

- [ ] **Step 7.7: Commit**

```bash
git add tools/replay.c tools/visualize_mask.py Makefile
chmod +x tools/visualize_mask.py
git commit -m "feat(tools): replay v2 mode + visualize_mask.py

build/replay auto-detects v1 vs v2 by reading meta.json.ui_version;
v2 mode calls tpv_process_frame_debug_v2 with meta's
tpv.bin_threshold / dark_object_mode / roi as runtime args.
CLI flags --v1 / --v2 / --bin-threshold / --dark-object-mode / --roi
override meta.json for diagnostic use. CSV grows two columns
bbox_area_px / grid_8x8; v1 rows leave them empty.

replay now compiles with -DTPV_DEBUG_FEATURES so tpv_process_frame_debug_v2
is linkable; this adds the v2 debug code path to the host binary only.

tools/visualize_mask.py parses a 38400 B .mask into a 640×480 grayscale
PNG (Pillow). Reports foreground pixel count for quick sanity."
```

---

## Task 8: DEVELOPER.md §11 v2 update

**Goal:** DEVELOPER.md §11 grows a "v2" subsection describing the new overlay, status line, Diag panel, Settings, `build/replay` v1/v2 mode, `visualize_mask.py`, and the migration note for v1 calibration data compatibility.

**Files:**
- Modify: `docs/DEVELOPER.md`

---

- [ ] **Step 8.1: Append v2 subsection to DEVELOPER.md §11**

Find the existing §11 "Android bench test APP" section's "### A2 p95 analysis" subsection. Append at the very end of §11 (before §12 or end-of-file):

```markdown

### v2 upgrade (2026-04-24)

The bench APP was upgraded from v1 to v2; see
`docs/specs/2026-04-24-bench-app-v2-mask-overlay-design.md` and
`docs/plans/2026-04-24-bench-app-v2-mask-overlay-plan.md`.

**What v2 adds on-screen:**
- Green translucent mask fill on the detected blob (replaces the v1 circle)
- Yellow ROI rectangle
- Status line above preview: `size:N [w×h] grid:M rotation:θ°`
- `Diag` button toggles a 2×3 panel of pipeline intermediate stages
  (raw Y / ROI dim / binarized / all CCL blobs / winning blob / last event)
- `ROI` button opens Settings to edit the ROI rect (Settings now run-locked)
- `Clear` button resets lastCommittedEvent + overlay + diag state

**New algorithm inputs (run-locked):**
- `binThreshold`: 0..255 cutoff (default 128)
- `darkObjectMode`: `true` means Y < threshold is foreground (default `true`;
  set for white-background-dark-object test scenes)
- `roi`: ROI rectangle in 640×480 coords (default full frame)

All are snapshotted at Start and written to `meta.json.tpv.{bin_threshold, dark_object_mode, roi}`.

**New per-event artifact:**
- `NNNNNN.mask` — 38400 B raw bitmap, LSB-first packed, 640×480. Set bits =
  winning blob pixels. Visualize with:

```bash
python3 tools/visualize_mask.py run/000001.mask
# → run/000001.png
```

**build/replay v1 vs v2 mode:**

| Mode | Triggered by | Algorithm | Threshold/ROI/dark source |
|---|---|---|---|
| v1 | `meta.json` missing or `ui_version` ≠ `"v2"`, or `--v1` flag | `tpv_process_frame` | compiled `tpv_bin_threshold`, no ROI, no dark mode |
| v2 | `meta.json.ui_version == "v2"`, or `--v2` flag | `tpv_process_frame_debug_v2` | from `meta.json.tpv.*`; CLI `--bin-threshold / --dark-object-mode / --roi x,y,w,h` override |

Example — replay a v2 run:
```bash
adb exec-out run-as com.tpv.bench cat files/runs/run_2026-04-24T07:30:00Z.zip > run.zip
unzip run.zip -d /tmp/run
./build/replay /tmp/run
```

**Calibration data and dark_object_mode:**

`src/model_data.c` is produced by PC-side `tools/calibrate` against a set
of training frames. The dark_object_mode setting used at calibration time
must match the setting used at run time. If you calibrate with bright
objects on dark background and then set `darkObjectMode=true` in the APP,
the model's Mahalanobis distance parameters will be meaningless. DEVELOPER
workflow: decide the physical scene first (dark-bg-bright-obj vs
bright-bg-dark-obj), then collect training frames, then set the matching
darkObjectMode in the APP's Settings before Start.

**Backward compatibility:**
- v2 APP continues to parse v1 runs (no `ui_version` in meta.json)
- v1 replay mode remains bit-identical to original v1 behavior
- HG1-5 C tests and the 20 KB size gate are unchanged
```

- [ ] **Step 8.2: Commit**

```bash
git add docs/DEVELOPER.md
git commit -m "docs(developer): §11 v2 upgrade note — overlay, mask artifact, replay modes"
```

---

## Task 9: Device acceptance — the 7 criteria from spec §9

**Goal:** Validate on the real Lenovo TB322FC device that the 7 spec §9 criteria hold:
1. Smoke: APK installs, green mask paints on white-bg-dark-object scene
2. Replay parity (v1 mode): v1 run's `.y` + replay `--v1` ≡ v1 log.jsonl
3. Replay parity (v2 mode): v2 run's `.y` + replay v2 mode ≡ log.jsonl (incl. bbox/area_px/grid_8x8)
4. Mask fidelity: `visualize_mask.py` shows blob shape matches object
5. Diagnostics panel: 6 cells render correctly
6. A2 ≤ 10 ms p95 (tighter than v1's 30 ms since v2 adds mask work)
7. ROI effective: shrinking ROI to exclude the object → detection fails

---

- [ ] **Step 9.1: Pre-flight — rebuild and SHA check**

```bash
export PATH="$HOME/android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin:$HOME/android/sdk/platform-tools:$PATH"
make clean && make android-apk && make android-verify-sha
```

Expected: `OK: APK model sha matches src/model_data.c (...)`.

- [ ] **Step 9.2: Install and launch on TB322FC**

```bash
adb -s HA25YBM0 install -r android/app/build/outputs/apk/debug/app-debug.apk
adb -s HA25YBM0 shell am start -n com.tpv.bench/.MainActivity
```

- [ ] **Step 9.3: Criterion 1 — Smoke**

Physical setup: white paper background, place a dark pen or phone in the scene.
On device: confirm Settings shows `darkObjectMode=true, binThreshold=128, roi=0,0,640,480`.
Tap Start. Grant CAMERA if prompted. Should see within 3 seconds:
- Green translucent fill over the dark object
- Red dot at object center
- Yellow ROI rect covering full frame
- Status line showing live `size:N [w×h] grid:M rotation:θ°`
- Green flash on commit; Events counter ≥ 1 after a few seconds

Place 3-4 more objects one at a time. Confirm each produces a COMMITTED event.

Tap Stop. Toast should show zip name.

- [ ] **Step 9.4: Criterion 2 — v1 replay parity**

If no v1 runs exist on-device, reinstall the v1 APK once (for continuity with v1). If a v1 run zip is available:

```bash
unzip -d /tmp/v1run path/to/v1_run.zip
./build/replay --v1 /tmp/v1run > /tmp/v1replay.csv
head /tmp/v1replay.csv
```

Compare with the v1 `log.jsonl` (parse `detection.{class_id, x, y, theta_x10, confidence_q8}` and match against CSV).

Skip this criterion if no v1 run is available — make a note in the commit.

- [ ] **Step 9.5: Criterion 3 — v2 replay parity**

```bash
adb -s HA25YBM0 exec-out run-as com.tpv.bench sh -c 'cat files/runs/run_<ts>.zip' > /tmp/v2run.zip
unzip -d /tmp/v2run /tmp/v2run.zip
./build/replay /tmp/v2run > /tmp/v2replay.csv
head /tmp/v2replay.csv
```

Extract event 1's expected fields:
```bash
head -1 /tmp/v2run/log.jsonl | python3 -c "
import json, sys
e = json.loads(sys.stdin.read())
d = e['detection']
print(f\"expected (frame {e['frame_idx_in_run']}): status=0 cls={d['class_id']} x={d['x']} y={d['y']} th={d['theta_x10']} conf={d['confidence_q8']} area={d['area_px']} grid={d['grid_8x8']}\")"
```

Locate `000001.y`'s row in the CSV and compare the 9 fields (status, class_id, x, y, theta_x10, confidence_q8, bbox_area_px, grid_8x8). All must match.

- [ ] **Step 9.6: Criterion 4 — mask visualization**

```bash
python3 tools/visualize_mask.py /tmp/v2run/000001.mask
```

Open the resulting PNG (or `open /tmp/v2run/000001.png` on macOS). Visually confirm the white region matches the shape of the object placed on camera. Foreground % reported on stdout should be consistent with the `area_px` from log.jsonl (38400 B × 8 bits = 307200 total pixels; area_px / 307200 = fg fraction).

- [ ] **Step 9.7: Criterion 5 — Diagnostics panel**

On device: tap `Diag` button. Should see 2×3 grid:
- (0,0) raw Y — grayscale live preview at low res
- (0,1) ROI — same as (0,0) but outside-ROI pixels dimmed to 40%
- (0,2) bin — black/white
- (1,0) all blobs — gray regions on dark bg
- (1,1) winner — green region on dark bg
- (1,2) last event — green region on dark bg (or all dark if no event yet)

Each cell labeled underneath. Tap Diag again to hide.

- [ ] **Step 9.8: Criterion 6 — A2 p95 ≤ 10 ms**

```bash
python3 tools/analyze_timing.py /tmp/v2run/timing.bin
```

Expected: the existing tool-level gate still reports `A2 gate (p95 ≤ 30 ms): PASS` with the actual p95 number. For v2 acceptance, record a separate manual verdict against the tighter target `p95 ≤ 10 ms`; PASS only if the printed p95 is ≤ 10 ms. v1 baseline was 4.84 ms; v2 adds ~3 bitmaps worth of memory ops (~3× 38400 bytes memset + O(n) fills) ≈ expected p95 ~7–9 ms.

- [ ] **Step 9.9: Criterion 7 — ROI exclusion**

On device: Settings → ROI = `0,0,100,100` (top-left 100×100 corner). Tap Stop if running. Start. Place object at image center. No events should trigger (the object is outside ROI → no blobs in CCL → TPV_EMPTY).

Move ROI back to `0,0,640,480` for subsequent testing. Object at center should once again trigger events.

- [ ] **Step 9.10: Record acceptance results in a new file**

Create `docs/acceptance/2026-04-24-v2-device-acceptance.md` with a short checklist of the 7 criteria and pass/fail notes + any timing numbers. Commit:

```bash
mkdir -p docs/acceptance
cat > docs/acceptance/2026-04-24-v2-device-acceptance.md <<'EOF'
# v2 Device Acceptance — Lenovo TB322FC

Date: 2026-04-24
Device: Lenovo TB322FC (Android 16 / arm64-v8a)

| # | Criterion | Result |
|---|---|---|
| 1 | Smoke — green mask paints, commits fire | <PASS/FAIL + notes> |
| 2 | v1 replay parity | <PASS / N/A if no v1 run> |
| 3 | v2 replay parity (9 fields match) | <PASS/FAIL> |
| 4 | Mask visualization (shape matches object) | <PASS/FAIL> |
| 5 | Diagnostics panel (6 cells) | <PASS/FAIL> |
| 6 | A2 p95 ≤ 10 ms | p95 = <N> ms |
| 7 | ROI exclusion (no event when obj outside ROI) | <PASS/FAIL> |

Notes: <anything unusual>
EOF
git add docs/acceptance/2026-04-24-v2-device-acceptance.md
git commit -m "acceptance: v2 device results on Lenovo TB322FC"
```

---

## Self-Review Checklist

### Spec coverage

Walk through `docs/specs/2026-04-24-bench-app-v2-mask-overlay-design.md` section by section:

| Spec § | Requirement | Plan task |
|---|---|---|
| §3 | v2 architecture diagram (arrows, new vs existing components) | T1–T6 (architecturally builds it out) |
| §4.1 | `tpv_DetectionDebugV2` struct fields | T1 Step 1.1 |
| §4.1 | `tpv_process_frame_debug_v2` signature including `bin_threshold` param | T1 Step 1.1 + 1.4 |
| §4.2 | `threshold_v2` helper reads threshold from arg not global | T1 Step 1.4 |
| §4.2 | Threshold dataflow: Settings → snapshot → JNI → C | T1 (snap in C) + T6 (Settings + snap) |
| §4.3 | ROI clipping `clip_bin_to_roi` post-threshold pre-CCL | T1 Step 1.4 |
| §4.4 | bin ⊇ all_blobs_mask ⊇ mask inclusion invariant | T1 Step 1.5 (test case) |
| §4.4 | `tpv_ccl_moments` gains optional `labels_out` | T1 Step 1.1 + 1.3 |
| §4.4 | v2 mask filled from `labels_out` + winner label | T1 Step 1.4 |
| §4.4 | scene_error / EMPTY / BAD_INPUT → all 3 masks zero | T1 Step 1.4 (top memset + explicit non-OK clears after threshold/CCL) |
| §4.5 | Production `tpv_process_frame` behavior unchanged | T1 Step 1.2 (only passes new NULL arg) |
| §4.6 | 5 v2 tests (parity, dark_mode, ROI, area, inclusion) | T1 Step 1.5 |
| §5.1 | Main screen layout (topbar, status, preview, hud) | T6 Step 6.2 |
| §5.1 | Status line format `size:N [w×h] grid:M rotation:θ°` | T6 Step 6.3 `updateHud` |
| §5.2 | OverlayView v2 with yellow ROI + green mask + red dot + axis | T4 Step 4.4 |
| §5.3 | DiagnosticsView 2×3 grid, 6 cells specified content | T5 Step 5.1 + 5.3 |
| §5.3 | 10 Hz throttle | T5 Step 5.3 |
| §5.4 | New buttons Diag / ROI / Clear | T6 Step 6.2 + 6.3 |
| §5.5 | SettingsState v2 fields + defaults (`darkObjectMode=true`) | T6 Step 6.1 |
| §5.5 | All v2 settings run-locked | T6 Step 6.3 lockUi + showSettingsDialog |
| §5.6 | renderOverlayJpeg v2 paints mask + ROI | T6 Step 6.3 |
| §6.1 | jsonl new fields: bbox, area_px, grid_8x8, artifacts.mask | T3 Step 3.6 |
| §6.2 | `.mask` file format (38400 B LSB-first) | T3 Step 3.6 |
| §6.3 | meta.json new fields (ui_version, dark_object_mode, roi) | T3 Step 3.4 + 3.5 |
| §6.4 | build/replay v1/v2 mode, CLI flags, CSV columns | T7 Step 7.1 + 7.2 |
| §6.5 | v1 ↔ v2 runs coexist based on ui_version | T7 Step 7.1 (defaults when ui_version absent) |
| §7 | Hard constraints (behavior, size, HG, parity, lock, thread) | T1 Step 1.9 + 1.10 |
| §8 | Implementation order T-v2.1 → T-v2.8 | Plan tasks T1-T9 |
| §9 | 7 device acceptance criteria | T9 Step 9.3 through 9.9 |
| §10 | open issues / defer | (no-op; spec-level tracking) |
| §11 | risks | covered by tests + A2 check in T9 |

No gaps.

### Placeholder scan

Searched the plan for the patterns listed in the writing-plans skill's "No Placeholders" section:
- No `TODO` / `TBD` / `implement later` outside of explicit "defer to v3" callouts in spec (which aren't placeholders — they're out-of-scope markers)
- Every "write a test" step includes the actual test code
- No "similar to Task N" references; code is repeated or delegated to earlier-tasks' type contracts
- Every step that changes code has a code block
- All types (`TpvBbox`, `TpvDetectionDebugV2`, `SettingsSnapshot`, `ReplayMeta`) defined in this plan before use

### Type consistency

Cross-checked naming across tasks:
- `TpvDetectionDebugV2` consistent across T2 (Kotlin def), T3 (TriggerMachine + RunRecorder upgrades), T4 (OverlayView), T5 (DiagnosticsRenderer), T6 (MainActivity), T7 (nope — C-side uses `tpv_DetectionDebugV2`), T9 (acceptance)
- `TpvBbox(x, y, w, h)` def in T2, used in T3/T5/T6
- `processFrameDebugV2` Kotlin external signature matches `Java_com_tpv_bench_TpvNative_processFrameDebugV2` JNI in T2
- `tpv_process_frame_debug_v2` C signature consistent T1 → JNI T2 → replay T7
- `GREEN_MASK_ARGB = 0x7800FF00` consistent T4 (OverlayPainter) + T4 (OverlayView onDraw) + T6 (renderOverlayJpeg). DiagnosticsRenderer uses the same 0xFF00FF00 for its winner tile — conceptually the same green, different alpha; spec §5.3 DiagnosticsView uses opaque green on dark bg for contrast, whereas overlay uses translucent. Intentional divergence, noted in code comments.
- `MASK_BYTES = 640 * 480 / 8 = 38400` used consistently everywhere (OverlayPainter, RunRecorder test, visualize_mask.py, tpv_jni MASK_LEN)
- `SettingsSnapshot` fields match `SettingsState` fields one-to-one (T6 Step 6.1 ↔ 6.3)
- `YuvAdapter.CropRect(x, y, w, h)` is the ROI type — reused, not a new class
- `FrameObservation` / `CommittedEvent` type upgrade (T3 Step 3.1) — verified TriggerMachine logic untouched

No mismatches.
