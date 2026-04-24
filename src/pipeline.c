#include <string.h>
#include "tpv_internal.h"
#include "tpv.h"

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
    int n = tpv_ccl_moments(g_bin, w, h, g_blobs, TPV_MAX_BLOBS);
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
#include "stdint.h"    /* INT32_MAX */

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
#endif  /* TPV_DEBUG_FEATURES */
