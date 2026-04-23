#include "tpv_internal.h"
#include <string.h>

/* Squared Mahalanobis distance: d² = ||L⁻¹ (x - μ)||²
 * All inputs/outputs in Q16.16. L_inv stored row-major in lower-triangular form. */
static int64_t mahal_sq_q16(const tpv_Features *x, const tpv_Template *tmpl) {
    int32_t dx[TPV_N_FEAT];
    const int32_t *xm = (const int32_t *)&tmpl->mean;
    const int32_t *xv = (const int32_t *)x;
    for (int i = 0; i < TPV_N_FEAT; i++) dx[i] = xv[i] - xm[i];

    int64_t y2 = 0;
    int idx = 0;
    for (int i = 0; i < TPV_N_FEAT; i++) {
        int64_t yi = 0;
        for (int j = 0; j <= i; j++) {
            yi += ((int64_t)tmpl->L_inv[idx + j] * dx[j]) >> 16;
        }
        idx += (i + 1);
        y2 += yi * yi;
    }
    return y2 >> 16;   /* Q32.32 -> Q16.16 */
}

void tpv_classify(const tpv_Features *f,
                  const tpv_Template *tmpl, int n,
                  uint8_t *class_id_out,
                  uint8_t *confidence_out,
                  int32_t *d1_sq_out) {
    int64_t d1sq = (int64_t)1 << 62;
    int64_t d2sq = (int64_t)1 << 62;
    int winner = 0;
    for (int c = 0; c < n; c++) {
        int64_t d = mahal_sq_q16(f, &tmpl[c]);
        if (d < d1sq)      { d2sq = d1sq; d1sq = d; winner = c; }
        else if (d < d2sq) { d2sq = d; }
    }
    *d1_sq_out = (int32_t)(d1sq > INT32_MAX ? INT32_MAX : d1sq);

    int32_t rt = tmpl[winner].reject_thresh;
    int32_t mg = tmpl[winner].margin;

    /* L3 closed boundary (spec §9.1): d1² ≥ rt → REJECTED */
    if (d1sq >= rt) {
        *class_id_out   = TPV_CLASS_REJECTED;
        *confidence_out = 0;
        return;
    }

    /* L3': only when N ≥ 2 and (d2² − d1²) < margin → AMBIGUOUS */
    int is_ambig = (n >= 2) && ((d2sq - d1sq) < mg);

    int32_t fit_q8 = (int32_t)((int64_t)255 * (rt - d1sq) / rt);
    if (fit_q8 < 0) fit_q8 = 0; else if (fit_q8 > 255) fit_q8 = 255;

    int32_t sep_q8;
    if (n < 2) {
        sep_q8 = 255;       /* HG1: single-class branch — no runner-up */
    } else {
        sep_q8 = (int32_t)((int64_t)255 * (d2sq - d1sq) / mg);
        if (sep_q8 < 0) sep_q8 = 0; else if (sep_q8 > 255) sep_q8 = 255;
    }

    int min_q8 = fit_q8 < sep_q8 ? fit_q8 : sep_q8;
    if (is_ambig) {
        *class_id_out   = TPV_CLASS_AMBIGUOUS;
        *confidence_out = (uint8_t)min_q8;
    } else {
        if (min_q8 < 1) min_q8 = 1;     /* ACCEPTED ⇒ ≥ 1 (spec §6) */
        *class_id_out   = (uint8_t)winner;
        *confidence_out = (uint8_t)min_q8;
    }
}
