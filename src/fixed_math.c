#include "tpv_internal.h"

/* Q16.16 integer square root via Newton's iteration.
 * At fixed point: r = ((x_q16 << 16) / r + r) / 2 satisfies r² = x_q16 << 16,
 * so r = sqrt(x_q16) in Q16.16.
 *
 * IMPORTANT: inputs > ~2^47 make `x_q16 << 16` overflow int64 inside the
 * Newton step. Use tpv_isqrt_i64 below for plain integer sqrt of large
 * values (e.g. eccentricity's discriminant). */
int64_t tpv_isqrt_q16(int64_t x_q16) {
    if (x_q16 <= 0) return 0;
    int64_t r = x_q16;
    for (int i = 0; i < 20; i++) {
        int64_t q = (x_q16 << 16) / r;
        r = (r + q) / 2;
    }
    return r;
}

/* Plain integer floor-sqrt: returns floor(sqrt(x)) for any non-negative int64.
 * Uses the same Newton iteration but without the << 16 scaling, so it's safe
 * for the full int64 range. Converges to the fixed point then stops. */
int64_t tpv_isqrt_i64(int64_t x) {
    if (x <= 0) return 0;
    int64_t r = x, last = -1;
    for (int i = 0; i < 64 && r != last; i++) {
        last = r;
        r = (r + x / r) / 2;
    }
    /* Newton can land on floor(sqrt)+1 for perfect-square-minus-epsilon inputs;
     * correct one step down if so. */
    while (r > 0 && r > x / r) r--;
    return r;
}

/* Q16.16 natural log. Decompose x = 2^shift * v with v ∈ [1, 2),
 * then ln(x) = shift·ln2 + ln(v). Compute ln(v) = ln(1+u) via 4-term Taylor:
 *   ln(1+u) ≈ u - u²/2 + u³/3 - u⁴/4   for u = v - 1 ∈ [0, 1). */
int32_t tpv_log_q16(int64_t x_q16) {
    if (x_q16 <= 0) return -(1 << 30);
    int shift = 0;
    int64_t v = x_q16;
    while (v >= (2LL << 16)) { v >>= 1; shift++; }
    while (v <  (1LL << 16)) { v <<= 1; shift--; }
    int64_t u  = v - (1LL << 16);
    int64_t u2 = (u  * u) >> 16;
    int64_t u3 = (u2 * u) >> 16;
    int64_t u4 = (u3 * u) >> 16;
    int64_t ln_v = u - (u2 >> 1) + (u3 / 3) - (u4 >> 2);
    const int64_t ln2_q16 = 45426;  /* round(ln(2) * 65536) */
    return (int32_t)((int64_t)shift * ln2_q16 + ln_v);
}

/* atan(i/64) * 65536 for i = 0..64, covering [0, π/4] in Q16.16.
 * Larger angles are reduced by quadrant + |y|<=|x| flip. */
static const int32_t tpv_atan_table[65] = {
        0,  1024,  2047,  3070,  4091,  5110,  6127,  7141,
     8152,  9159, 10162, 11162, 12157, 13146, 14131, 15110,
    16084, 17051, 18012, 18966, 19913, 20853, 21785, 22710,
    23627, 24535, 25436, 26327, 27210, 28083, 28947, 29802,
    30647, 31481, 32306, 33120, 33924, 34717, 35499, 36270,
    37030, 37779, 38517, 39244, 39960, 40665, 41359, 42043,
    42716, 43378, 44030, 44671, 45302, 45923, 46534, 47135,
    47726, 48308, 48880, 49443, 49997, 50542, 51078, 51606,
    52125
};

/* Q16.16 atan2 in radians, range (-π, π].
 * Take |x|, |y|, put smaller on top so ratio stays in [0,1] -> table lookup;
 * unflip via π/2 - a if swapped, π - a if x was negative, negate if y was negative. */
int32_t tpv_atan2_q16(int64_t y, int64_t x) {
    if (x == 0 && y == 0) return 0;
    int sign_y = (y < 0); int64_t ay = sign_y ? -y : y;
    int sign_x = (x < 0); int64_t ax = sign_x ? -x : x;
    int swap = (ay > ax);
    int64_t n = swap ? ax : ay;
    int64_t d = swap ? ay : ax;
    int idx = d == 0 ? 64 : (int)((n * 64) / d);
    if (idx > 64) idx = 64;
    int32_t ang = tpv_atan_table[idx];
    if (swap)   ang = (int32_t)(102944 - ang);   /* π/2 ≈ 102944 in Q16.16 */
    if (sign_x) ang = (int32_t)(205887 - ang);   /* π   ≈ 205887 in Q16.16 */
    if (sign_y) ang = -ang;
    return ang;
}
