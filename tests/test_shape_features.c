#include <string.h>
#include "tpv_internal.h"
#include "testlib.h"

static tpv_Blob make_square(void) {
    tpv_Blob b = {0};
    b.m00 = 16;
    b.m10 = 24; b.m01 = 24;          /* centroid (1.5, 1.5) */
    b.mu20 = 20; b.mu02 = 20; b.mu11 = 0;
    b.mu30 = 0; b.mu03 = 0; b.mu21 = 0; b.mu12 = 0;
    b.perimeter = 16;
    b.bbox_x0 = 0; b.bbox_x1 = 3; b.bbox_y0 = 0; b.bbox_y1 = 3;
    return b;
}

TEST(t_square_features_symmetric) {
    tpv_Blob b = make_square();
    tpv_Features f;
    tpv_shape_features(&b, &f);
    /* Square's 2nd-order Hu invariant is non-zero (η₂₀ + η₀₂). */
    CHECK(f.hu[0] != 0);
    /* Square has zero 3rd-order moments → sign degenerates to 0. */
    CHECK_EQ_I(f.m3_axis_sign, 0);
    /* Eccentricity ≈ 0 for a perfect square (allow ±1e-3 in Q16.16). */
    CHECK(f.eccentricity >= -65 && f.eccentricity <= 65);
}

TEST(t_large_blob_no_eccentricity_overflow) {
    /* Regression: a legal blob (m00 ≤ Amax = 50000) shaped like a two-axis
     * dumbbell puts μ₂₀ and μ₀₂ both in the gigapoint range. Without
     * scaling, `tr = μ₂₀ + μ₀₂` squares past int64.
     *
     * Choose μ₂₀ = 4e9, μ₀₂ = 1e9, μ₁₁ = 0 so tr = 5e9 and tr*tr = 2.5e19 —
     * strictly above INT64_MAX (9.2e18) — while still being physically
     * reachable (two 25000-px clumps at ±320 px ≈ μ₂₀ = 5.1e9). The
     * expected eccentricity is deterministic:
     *     λ_max = μ₂₀ = 4e9
     *     λ_min = μ₀₂ = 1e9
     *     ecc   = sqrt(1 - λ_min/λ_max) = sqrt(3/4) ≈ 0.8660
     * in Q16.16 that's 56756 ± a few LSB.
     *
     * If k_shift scaling or the `tpv_isqrt_i64` step ever regresses, tr*tr
     * wraps, disc is garbage, and ecc lands far from 0.866 — caught here. */
    tpv_Blob b = {0};
    b.m00  = 50000;
    b.m10  = 50000 * 320;
    b.m01  = 50000 * 240;
    b.mu20 = 4000000000LL;    /* 4e9 */
    b.mu02 = 1000000000LL;    /* 1e9 */
    b.mu11 = 0;
    b.perimeter = 500;
    b.bbox_x0 = 0; b.bbox_x1 = 639;
    b.bbox_y0 = 0; b.bbox_y1 = 479;

    /* Precondition: tr*tr must actually overflow int64 under the naïve path,
     * or this test is vacuous (as a prior version was, sitting at tr=2.4e9).
     * sqrt(INT64_MAX) ≈ 3.037e9; any tr strictly above that forces tr*tr
     * past INT64_MAX. We assert the threshold directly rather than computing
     * tr*tr and inspecting the wrap: signed-integer overflow is UB, so a
     * compiler (or UBSan) is free to elide or trap on that multiply. */
    int64_t tr_check = b.mu20 + b.mu02;
    CHECK(tr_check > 3037000499LL);

    tpv_Features f;
    tpv_shape_features(&b, &f);

    /* Expected ecc ≈ 0.8660 in Q16.16 = 56756. Allow ±1% = ±656 LSB for
     * fixed-point rounding throughout the pipeline. */
    int32_t expected = (int32_t)(0.8660 * 65536);
    CHECK(f.eccentricity >= expected - 700);
    CHECK(f.eccentricity <= expected + 700);
}

int main(void) {
    RUN(t_square_features_symmetric);
    RUN(t_large_blob_no_eccentricity_overflow);
    FINISH();
}
