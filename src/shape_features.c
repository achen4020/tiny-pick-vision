#include "tpv_internal.h"

/* Compute the 10-D feature vector from a Blob's accumulated moments.
 * 7 log-Hu invariants + perim_ratio + eccentricity + m3_axis_sign. */
void tpv_shape_features(const tpv_Blob *b, tpv_Features *f) {
    int64_t m00 = b->m00;
    if (m00 == 0) {
        for (int i = 0; i < 10; i++) ((int32_t*)f)[i] = 0;
        return;
    }

    /* Normalize central moments: η_pq = μ_pq / m00^((p+q)/2 + 1).
     * Output magnitude in Q16.16 (the absolute scale doesn't matter
     * for log-compressed Hu features used in Mahalanobis distance). */
    int64_t m00sq = m00 * m00;
    int64_t n20 = (b->mu20 << 16) / m00sq;
    int64_t n02 = (b->mu02 << 16) / m00sq;
    int64_t n11 = (b->mu11 << 16) / m00sq;
    int64_t sqrt_m00_q16 = tpv_isqrt_q16(m00 << 16);
    int64_t m00_25 = (m00sq * sqrt_m00_q16) >> 16;
    if (m00_25 == 0) m00_25 = 1;
    int64_t n30 = (b->mu30 << 16) / m00_25;
    int64_t n21 = (b->mu21 << 16) / m00_25;
    int64_t n12 = (b->mu12 << 16) / m00_25;
    int64_t n03 = (b->mu03 << 16) / m00_25;

    /* Hu 1962 seven invariants. We keep the algebraic form; absolute scale
     * doesn't matter because each entry is log-compressed below. */
    int64_t h[7];
    int64_t a = n20 - n02;
    int64_t s30_3_12 = n30 - 3*n12;
    int64_t t3_21_03 = 3*n21 - n03;
    int64_t s30_p_12 = n30 + n12;
    int64_t s21_p_03 = n21 + n03;

    h[0] = n20 + n02;
    h[1] = a*a + 4*n11*n11;
    h[2] = s30_3_12*s30_3_12 + t3_21_03*t3_21_03;
    h[3] = s30_p_12*s30_p_12 + s21_p_03*s21_p_03;
    h[4] = s30_3_12 * s30_p_12 *
           (s30_p_12*s30_p_12 - 3*s21_p_03*s21_p_03)
         + t3_21_03 * s21_p_03 *
           (3*s30_p_12*s30_p_12 - s21_p_03*s21_p_03);
    h[5] = a * (s30_p_12*s30_p_12 - s21_p_03*s21_p_03)
         + 4 * n11 * s30_p_12 * s21_p_03;
    h[6] = t3_21_03 * s30_p_12 *
           (s30_p_12*s30_p_12 - 3*s21_p_03*s21_p_03)
         - s30_3_12 * s21_p_03 *
           (3*s30_p_12*s30_p_12 - s21_p_03*s21_p_03);

    for (int i = 0; i < 7; i++) {
        int64_t ax = h[i] < 0 ? -h[i] : h[i];
        int32_t sign = h[i] < 0 ? -1 : 1;
        int32_t lg = tpv_log_q16(ax + 1);   /* +1 protects log(0) */
        f->hu[i] = sign * lg;
    }

    /* perim_ratio = perimeter / sqrt(area), in Q16.16. */
    int64_t sqrt_area = tpv_isqrt_q16(m00 << 16);
    if (sqrt_area == 0) sqrt_area = 1;
    f->perim_ratio = (int32_t)(((int64_t)b->perimeter << 32) / sqrt_area);

    /* Eccentricity from 2×2 covariance eigenvalues:
     *   λ = (tr ± sqrt(tr² - 4·det)) / 2,  ecc = sqrt(1 - λmin/λmax). */
    int64_t tr = b->mu20 + b->mu02;
    int64_t det = b->mu20 * b->mu02 - b->mu11 * b->mu11;
    int64_t disc = tr*tr - 4*det;
    int64_t sdisc = disc > 0 ? tpv_isqrt_q16(disc << 16) : 0;
    int64_t l1 = (tr + (sdisc >> 16)) / 2;
    int64_t l2 = (tr - (sdisc >> 16)) / 2;
    if (l1 < l2) { int64_t tmp = l1; l1 = l2; l2 = tmp; }
    if (l1 == 0) {
        f->eccentricity = 0;
    } else {
        int64_t ratio = (l2 << 16) / l1;     /* Q16.16, ∈ [0, 1] */
        int64_t one_q16 = (int64_t)1 << 16;
        int64_t one_minus_ratio = one_q16 - ratio;
        if (one_minus_ratio < 0) one_minus_ratio = 0;
        f->eccentricity = (int32_t)tpv_isqrt_q16(one_minus_ratio << 16);
    }

    /* m3_axis_sign: simplified — full version projects μ₃ onto principal axis.
     * For symmetric shapes (4×4 square: all μ_3 = 0) this gives 0 as required. */
    int64_t proj_simple = b->mu30 + b->mu03;
    if (proj_simple > TPV_M3_EPS)        f->m3_axis_sign =  1;
    else if (proj_simple < -TPV_M3_EPS)  f->m3_axis_sign = -1;
    else                                  f->m3_axis_sign =  0;
}
