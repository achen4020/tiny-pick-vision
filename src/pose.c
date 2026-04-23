#include "tpv_internal.h"

/* Q16.16 constant for π. round(π * 65536) = 205887. */
#define TPV_PI_Q16   205887

void tpv_pose(const tpv_Blob *b,
              int16_t *x_out, int16_t *y_out, int16_t *theta_x10_out) {
    if (b->m00 == 0) { *x_out = *y_out = *theta_x10_out = 0; return; }
    *x_out = (int16_t)(b->m10 / b->m00);
    *y_out = (int16_t)(b->m01 / b->m00);

    /* Principal-axis angle: θ = ½ · atan2(2·μ₁₁, μ₂₀ − μ₀₂).
     * Result is in [-π/2, π/2]; the 180° disambiguation (spec §7) is
     * deferred — see the m3_axis_sign comment in shape_features.c.
     * Robot integrators must resolve the 180° ambiguity downstream. */
    int64_t num = 2 * b->mu11;
    int64_t den = b->mu20 - b->mu02;
    int32_t theta_q16 = tpv_atan2_q16(num, den) / 2;

    /* Convert Q16.16 radians → degrees × 10. */
    int64_t tmp = (int64_t)theta_q16 * 1800;
    int32_t deg_x10 = (int32_t)(tmp / TPV_PI_Q16);
    /* Normalize to [-1800, 1800). */
    while (deg_x10 < -1800) deg_x10 += 3600;
    while (deg_x10 >= 1800) deg_x10 -= 3600;
    *theta_x10_out = (int16_t)deg_x10;
}
