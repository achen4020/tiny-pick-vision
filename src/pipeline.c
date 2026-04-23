#include <string.h>
#include "tpv_internal.h"
#include "tpv.h"

/* Per-frame scratch in .bss (no heap, no large stack). */
static uint8_t       g_bin[TPV_WIDTH * TPV_HEIGHT / 8];
static tpv_Blob      g_blobs[TPV_MAX_BLOBS];
static tpv_Detection g_pool[TPV_MAX_BLOBS];
static int32_t       g_d1sq_pool[TPV_MAX_BLOBS];

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
    if (best_acc    >= 0) { *det_out = g_pool[best_acc];    return TPV_OK; }
    if (best_reject >= 0) { *det_out = g_pool[best_reject]; return TPV_OK; }
    return TPV_EMPTY;
}
