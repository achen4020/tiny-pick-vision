#include <string.h>
#include "tpv_internal.h"
#include "tpv.h"

/* Threading contract: this module's file-static scratch (pre-existing
 * g_bin/g_blobs/g_pool/g_d1sq_pool plus debug-only g_features_pool and
 * s_last_winner_*) is NOT re-entrant. Callers must serialize
 * tpv_process_frame() and tpv_process_frame_debug() invocations on a
 * single thread. The bench-test APP (spec §3) pins the whole
 * camera → JNI → recorder chain onto a single executor; the C library
 * by itself imposes no synchronization. */

/* Per-frame scratch in .bss (no heap, no large stack). */
static uint8_t       g_bin[TPV_WIDTH * TPV_HEIGHT / 8];
static tpv_Blob      g_blobs[TPV_MAX_BLOBS];
static tpv_Detection g_pool[TPV_MAX_BLOBS];
static int32_t       g_d1sq_pool[TPV_MAX_BLOBS];

#ifdef TPV_DEBUG_FEATURES
/* Parallel to g_pool[]: stores the 10-D feature vector that produced each
 * blob's Detection, so tpv_process_frame_debug can read the winner's
 * features back after tpv_process_frame has made its selection. */
static tpv_Features g_features_pool[TPV_MAX_BLOBS];
/* Features of the blob chosen as the final output of tpv_process_frame.
 * Valid only when the latest tpv_process_frame call returned TPV_OK. */
static tpv_Features s_last_winner_features;
static int          s_last_winner_valid;
#endif

int tpv_process_frame(const uint8_t *y, int w, int h, tpv_Detection *det_out) {
    if (!y || !det_out || w != TPV_WIDTH || h != TPV_HEIGHT) return TPV_BAD_INPUT;
    memset(det_out, 0, sizeof *det_out);
    memset(g_bin, 0, sizeof g_bin);

    tpv_threshold(y, w, h, g_bin);
    int n = tpv_ccl_moments(g_bin, w, h, g_blobs, TPV_MAX_BLOBS, NULL);
    if (n < 0) return TPV_SCENE_ERROR;

    /* Geometric filter + pose + classify per surviving blob. */
    int pn = 0;
    for (int i = 0; i < n; i++) {
        if (g_blobs[i].m00 < TPV_AMIN || g_blobs[i].m00 > TPV_AMAX) continue;
        tpv_Features f;
        tpv_shape_features(&g_blobs[i], &f);
        tpv_Detection d = {0};
        uint8_t cid = 0, conf = 0; int32_t d1sq = 0;
        tpv_classify(&f, tpv_templates, TPV_N_CLASSES, &cid, &conf, &d1sq);
        tpv_pose(&g_blobs[i], &d.x, &d.y, &d.theta_x10);
        d.class_id = cid; d.confidence_q8 = conf;
        g_pool[pn] = d;
        g_d1sq_pool[pn] = d1sq;
#ifdef TPV_DEBUG_FEATURES
        g_features_pool[pn] = f;     /* the tpv_Features 'f' from line above */
#endif
        pn++;
    }
    if (pn == 0) return TPV_EMPTY;

    /* Final selection (spec §5):
     *   1) argmax confidence over ACCEPTED blobs (class_id ∈ {0..4}), OR
     *   2) min d1² over AMBIGUOUS/REJECTED blobs as fallback (operator-visible
     *      "we saw something but couldn't classify"), OR
     *   3) TPV_EMPTY if neither.
     * Critical: a high-confidence REJECTED MUST NOT beat a low-confidence ACCEPTED. */
    int best_acc = -1, best_conf = -1;
    int best_reject = -1; int32_t best_d1 = INT32_MAX;
    for (int i = 0; i < pn; i++) {
        uint8_t cid = g_pool[i].class_id;
        if (cid <= 4) {
            if (g_pool[i].confidence_q8 > best_conf) {
                best_conf = g_pool[i].confidence_q8;
                best_acc = i;
            }
        } else {
            if (g_d1sq_pool[i] < best_d1) {
                best_d1 = g_d1sq_pool[i];
                best_reject = i;
            }
        }
    }
#ifdef TPV_DEBUG_FEATURES
    s_last_winner_valid = 0;
#endif
    if (best_acc >= 0) {
        *det_out = g_pool[best_acc];
#ifdef TPV_DEBUG_FEATURES
        s_last_winner_features = g_features_pool[best_acc];
        s_last_winner_valid = 1;
#endif
        return TPV_OK;
    }
    if (best_reject >= 0) {
        *det_out = g_pool[best_reject];
#ifdef TPV_DEBUG_FEATURES
        s_last_winner_features = g_features_pool[best_reject];
        s_last_winner_valid = 1;
#endif
        return TPV_OK;
    }
    return TPV_EMPTY;
}

#ifdef TPV_DEBUG_FEATURES
/* Debug variant: first runs the production path (so the decision is
 * byte-identical), then reads back the winning blob's features from the
 * module-static stash and computes per-template d² by looping over
 * tpv_templates. Never compiled into the production .so (the 20 KB size
 * gate stays intact).
 *
 * Determinism note: we memset the ENTIRE `out` struct up front (not just
 * features/distances_sq). tpv_process_frame early-returns TPV_BAD_INPUT
 * *before* it memsets its det_out (see src/pipeline.c:12), so without this
 * top-level clear the caller would see indeterminate det fields on
 * TPV_BAD_INPUT. With it, the JNI-side promise "even on non-OK rc we
 * still construct a valid TpvDetectionDebug" holds for every code path. */
int tpv_process_frame_debug(const uint8_t *y, int w, int h,
                            tpv_DetectionDebug *out) {
    if (!out) return TPV_BAD_INPUT;
    memset(out, 0, sizeof *out);

    int rc = tpv_process_frame(y, w, h, &out->det);
    if (rc != TPV_OK) return rc;
    if (!s_last_winner_valid) return rc;   /* can't happen when rc == OK */

    out->features = s_last_winner_features;
    for (int c = 0; c < TPV_N_CLASSES; c++) {
        int64_t d = tpv_mahal_sq_q16(&out->features, &tpv_templates[c]);
        out->distances_sq[c] = (int32_t)(d > INT32_MAX ? INT32_MAX : d);
    }
    return TPV_OK;
}

/* v2 extras: runtime-tunable threshold + direction, ROI clipping, and
 * per-frame label map buffer for mask derivation. All static + #ifdef-
 * guarded; production build sees none of this. */
static uint16_t g_labels_v2[TPV_WIDTH * TPV_HEIGHT];
static uint8_t  g_strong_bin_v2[TPV_WIDTH * TPV_HEIGHT / 8];
static uint8_t  g_display_core_bin_v2[TPV_WIDTH * TPV_HEIGHT / 8];
static uint8_t  g_seeded_label_v2[TPV_MAX_BLOBS + 1];
static uint8_t  g_border_label_v2[TPV_MAX_BLOBS + 1];
static uint8_t  g_group_label_v2[TPV_MAX_BLOBS + 1];

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

static uint8_t relaxed_threshold_v2(uint8_t threshold, int dark_mode) {
    const int delta = 48;
    int t = dark_mode ? (int)threshold + delta : (int)threshold - delta;
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    return (uint8_t)t;
}

static uint8_t display_core_threshold_v2(uint8_t threshold, int dark_mode) {
    const int delta = 32;
    int t = dark_mode ? (int)threshold - delta : (int)threshold + delta;
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    return (uint8_t)t;
}

static int bit_is_set_v2(const uint8_t *bits, int idx) {
    return (bits[idx >> 3] >> (idx & 7)) & 1;
}

static void set_bit_v2(uint8_t *bits, int idx) {
    bits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

static int ranges_overlap_v2(int a0, int a1, int b0, int b1) {
    return a0 <= b1 && b0 <= a1;
}

static int bboxes_near_v2(const tpv_Blob *a, const tpv_Blob *b) {
    const int margin_x = 80;
    const int margin_y = 80;
    int ax0 = a->bbox_x0 - margin_x, ax1 = a->bbox_x1 + margin_x;
    int ay0 = a->bbox_y0 - margin_y, ay1 = a->bbox_y1 + margin_y;
    return ranges_overlap_v2(ax0, ax1, b->bbox_x0, b->bbox_x1) &&
           ranges_overlap_v2(ay0, ay1, b->bbox_y0, b->bbox_y1);
}

static void fill_mask_spans_v2(uint8_t *mask, int w, int h,
                               int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;

    /* Horizontal span fill: closes holes on rows that already intersect the
     * object mask. */
    for (int yy = y0; yy <= y1; yy++) {
        int first = -1, last = -1;
        for (int xx = x0; xx <= x1; xx++) {
            int idx = yy * w + xx;
            if (bit_is_set_v2(mask, idx)) {
                if (first < 0) first = xx;
                last = xx;
            }
        }
        if (first >= 0) {
            for (int xx = first; xx <= last; xx++) {
                set_bit_v2(mask, yy * w + xx);
            }
        }
    }

    /* Vertical span fill: closes highlight bands that split the same physical
     * object into top/bottom pieces. */
    for (int xx = x0; xx <= x1; xx++) {
        int first = -1, last = -1;
        for (int yy = y0; yy <= y1; yy++) {
            int idx = yy * w + xx;
            if (bit_is_set_v2(mask, idx)) {
                if (first < 0) first = yy;
                last = yy;
            }
        }
        if (first >= 0) {
            for (int yy = first; yy <= last; yy++) {
                set_bit_v2(mask, yy * w + xx);
            }
        }
    }

    /* Do not fill the whole bbox. Real phone-like targets are slightly
     * rotated and have rounded corners; rectangular envelope fill makes the
     * green overlay spill onto the white background. Row/column spans close
     * internal highlight gaps while preserving sloped outer edges. */
}

static void measure_mask_v2(const uint8_t *mask, int w,
                            int x0, int y0, int x1, int y1,
                            int32_t *area_out,
                            int64_t *m10_out,
                            int64_t *m01_out) {
    int32_t area = 0;
    int64_t m10 = 0, m01 = 0;
    for (int yy = y0; yy <= y1; yy++) {
        for (int xx = x0; xx <= x1; xx++) {
            int idx = yy * w + xx;
            if (bit_is_set_v2(mask, idx)) {
                area++;
                m10 += xx;
                m01 += yy;
            }
        }
    }
    *area_out = area;
    *m10_out = m10;
    *m01_out = m01;
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
            /* Is any pixel in this 8x8 block foreground? */
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
    dark_object_mode = !!dark_object_mode;   /* normalize to 0 or 1 */

    if (!y || w != TPV_WIDTH || h != TPV_HEIGHT) return TPV_BAD_INPUT;
    /* Overflow-safe ROI bounds check: since roi_x >= 0 and roi_w >= 1 are
     * already guaranteed by the preceding clause, (w - roi_x) cannot
     * underflow; writing the bound as `roi_w > w - roi_x` avoids the
     * signed-int-overflow hazard of `roi_x + roi_w` for pathological
     * roi_w near INT_MAX. Same reasoning for h. */
    if (roi_x < 0 || roi_y < 0 || roi_w <= 0 || roi_h <= 0 ||
        roi_w > w - roi_x || roi_h > h - roi_y) return TPV_BAD_INPUT;

    /* 1. Strong seeds from the user-visible threshold. */
    threshold_v2(y, w, h, bin_threshold, dark_object_mode, g_strong_bin_v2);
    clip_bin_to_roi(g_strong_bin_v2, w, h, roi_x, roi_y, roi_w, roi_h);

    /* 2. Weak candidate mask for glossy/printed dark objects. The final bin
     * keeps only weak connected components that contain at least one strong
     * seed. This is hysteresis thresholding: it bridges reflections inside an
     * object without accepting unrelated weak background regions. */
    threshold_v2(y, w, h, relaxed_threshold_v2(bin_threshold, dark_object_mode),
                 dark_object_mode, out->bin);
    clip_bin_to_roi(out->bin, w, h, roi_x, roi_y, roi_w, roi_h);

    /* 3. CCL with label map */
    static tpv_Blob blobs[TPV_MAX_BLOBS];
    int n = tpv_ccl_moments(out->bin, w, h, blobs, TPV_MAX_BLOBS, g_labels_v2);
    if (n < 0) {
        /* If the relaxed weak threshold admits too many unrelated tiny
         * components, preserve the old strong-threshold behavior rather than
         * turning a previously detectable frame into SCENE_ERROR. */
        memcpy(out->bin, g_strong_bin_v2, sizeof out->bin);
        n = tpv_ccl_moments(out->bin, w, h, blobs, TPV_MAX_BLOBS, g_labels_v2);
        if (n < 0) {
            /* Non-OK contract: BAD_INPUT / SCENE_ERROR / EMPTY expose no stale
             * diagnostic masks. Clear bin too, even though thresholding ran. */
            memset(out, 0, sizeof *out);
            return TPV_SCENE_ERROR;
        }
    }

    memset(g_seeded_label_v2, 0, sizeof g_seeded_label_v2);
    memset(g_border_label_v2, 0, sizeof g_border_label_v2);
    const int npix = w * h;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int i = yy * w + xx;
            uint16_t label = g_labels_v2[i];
            if (label == 0) continue;
            if (bit_is_set_v2(g_strong_bin_v2, i)) {
                g_seeded_label_v2[label] = 1;
            }
            if (xx == 0 || yy == 0 || xx == w - 1 || yy == h - 1) {
                g_border_label_v2[label] = 1;
            }
        }
    }

    /* 4. Rewrite out->bin to the hysteresis-filtered weak components, and
     * fill all_blobs_mask from the same kept components. Frame-border
     * components are usually table/background, not the centered object under
     * test, so drop them from the bench debug overlay. */
    memset(out->bin, 0, sizeof out->bin);
    for (int i = 0; i < npix; i++) {
        uint16_t label = g_labels_v2[i];
        if (label != 0 && g_seeded_label_v2[label] && !g_border_label_v2[label]) {
            out->bin[i >> 3] |= (uint8_t)(1u << (i & 7));
            out->all_blobs_mask[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }

    /* 5. Geometric + classification - same decision as v1.
     * tpv_process_frame re-thresholds from scratch, so we can't delegate
     * to it here; duplicate the filter/pose/classify/argmax loop inline
     * over the blobs[] we just computed. */
    static tpv_Detection pool[TPV_MAX_BLOBS];
    static int32_t       d1_pool[TPV_MAX_BLOBS];
    static tpv_Features  feat_pool[TPV_MAX_BLOBS];
    static int           blob_idx_pool[TPV_MAX_BLOBS];
    int pn = 0;
    int32_t area_pool[TPV_MAX_BLOBS];
    /* NOTE: the per-blob filter/classify/pose/argmax loop below duplicates
     * the same logic in tpv_process_frame (src/pipeline.c:~22-58). Any
     * change to the production selection policy (AMIN/AMAX bounds, argmax
     * tie-break, classifier args, etc.) MUST be mirrored in both places.
     * A cleaner refactor into a shared static helper is left for v3 — v1
     * function re-thresholds from scratch, so extracting a true common
     * helper would require larger restructuring. */
    for (int i = 0; i < n; i++) {
        if (!g_seeded_label_v2[i + 1]) continue;
        if (g_border_label_v2[i + 1]) continue;
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
        area_pool[pn] = blobs[i].m00;
        pn++;
    }
    if (pn == 0) {
        memset(out, 0, sizeof *out);
        return TPV_EMPTY;
    }

    /* argmax over ACCEPTED, else min d1 over REJECTED/AMBIGUOUS */
    int best_acc = -1, best_conf = -1;
    int best_reject = -1; int32_t best_reject_area = -1;
    int32_t best_d1 = INT32_MAX;
    for (int i = 0; i < pn; i++) {
        if (pool[i].class_id <= 4) {
            if (pool[i].confidence_q8 > best_conf) {
                best_conf = pool[i].confidence_q8;
                best_acc = i;
            }
        } else {
            if (area_pool[i] > best_reject_area ||
                (area_pool[i] == best_reject_area && d1_pool[i] < best_d1)) {
                best_reject_area = area_pool[i];
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

    memset(g_group_label_v2, 0, sizeof g_group_label_v2);
    g_group_label_v2[winner_label] = 1;
    int32_t group_area = 0;
    int64_t group_m10 = 0, group_m01 = 0;
    int group_x0 = TPV_WIDTH, group_y0 = TPV_HEIGHT;
    int group_x1 = -1, group_y1 = -1;
    for (int i = 0; i < n; i++) {
        uint16_t label = (uint16_t)(i + 1);
        if (!g_seeded_label_v2[label]) continue;
        if (g_border_label_v2[label]) continue;
        if (blobs[i].m00 < TPV_AMIN) continue;
        if (!bboxes_near_v2(&blobs[winner_blob], &blobs[i])) continue;
        g_group_label_v2[label] = 1;
        group_area += blobs[i].m00;
        group_m10 += blobs[i].m10;
        group_m01 += blobs[i].m01;
        if (blobs[i].bbox_x0 < group_x0) group_x0 = blobs[i].bbox_x0;
        if (blobs[i].bbox_y0 < group_y0) group_y0 = blobs[i].bbox_y0;
        if (blobs[i].bbox_x1 > group_x1) group_x1 = blobs[i].bbox_x1;
        if (blobs[i].bbox_y1 > group_y1) group_y1 = blobs[i].bbox_y1;
    }
    if (group_area <= 0) {
        g_group_label_v2[winner_label] = 1;
        group_area = blobs[winner_blob].m00;
        group_m10 = blobs[winner_blob].m10;
        group_m01 = blobs[winner_blob].m01;
        group_x0 = blobs[winner_blob].bbox_x0;
        group_y0 = blobs[winner_blob].bbox_y0;
        group_x1 = blobs[winner_blob].bbox_x1;
        group_y1 = blobs[winner_blob].bbox_y1;
    }

    out->det = pool[winner_pn];
    if (group_area > 0) {
        out->det.x = (int16_t)(group_m10 / group_area);
        out->det.y = (int16_t)(group_m01 / group_area);
    }
    out->features = feat_pool[winner_pn];
    for (int c = 0; c < TPV_N_CLASSES; c++) {
        int64_t d = tpv_mahal_sq_q16(&out->features, &tpv_templates[c]);
        out->distances_sq[c] = (int32_t)(d > INT32_MAX ? INT32_MAX : d);
    }

    out->bbox_x0 = (int16_t)group_x0;
    out->bbox_y0 = (int16_t)group_y0;
    out->bbox_x1 = (int16_t)group_x1;
    out->bbox_y1 = (int16_t)group_y1;
    out->area_px = group_area;

    /* 6. Fill final object mask: winner anchor plus nearby non-border
     * components that likely belong to the same physical object. */
    threshold_v2(y, w, h, display_core_threshold_v2(bin_threshold, dark_object_mode),
                 dark_object_mode, g_display_core_bin_v2);
    clip_bin_to_roi(g_display_core_bin_v2, w, h, roi_x, roi_y, roi_w, roi_h);
    int display_core_px = 0;
    for (int i = 0; i < npix; i++) {
        uint16_t label = g_labels_v2[i];
        if (label != 0 && g_group_label_v2[label] &&
            bit_is_set_v2(g_display_core_bin_v2, i)) {
            out->mask[i >> 3] |= (uint8_t)(1u << (i & 7));
            display_core_px++;
        }
    }
    if (display_core_px < TPV_AMIN) {
        memset(out->mask, 0, sizeof out->mask);
        for (int i = 0; i < npix; i++) {
            uint16_t label = g_labels_v2[i];
            if (label != 0 && g_group_label_v2[label] &&
                bit_is_set_v2(g_strong_bin_v2, i)) {
                out->mask[i >> 3] |= (uint8_t)(1u << (i & 7));
            }
        }
    }

    fill_mask_spans_v2(out->mask, w, h, group_x0, group_y0, group_x1, group_y1);
    for (int i = 0; i < npix; i++) {
        if (bit_is_set_v2(out->mask, i)) {
            out->bin[i >> 3] |= (uint8_t)(1u << (i & 7));
            out->all_blobs_mask[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }
    measure_mask_v2(out->mask, w, group_x0, group_y0, group_x1, group_y1,
                    &out->area_px, &group_m10, &group_m01);
    if (out->area_px > 0) {
        out->det.x = (int16_t)(group_m10 / out->area_px);
        out->det.y = (int16_t)(group_m01 / out->area_px);
    }

    /* 7. grid_8x8 from mask */
    compute_grid_8x8(out->mask, w, h, &out->grid_8x8);

    return TPV_OK;
}
#endif  /* TPV_DEBUG_FEATURES */
