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
    /* Regression: a legal blob (m00 ≤ Amax = 50000) spanning much of the
     * frame pushes μ₂₀ to ~5e9, so the naïve tr·tr computation overflows
     * int64 and produces garbage eccentricity. This test exercises the
     * overflow path by feeding synthetic moments at the upper bound.
     *
     * The values below correspond to a horizontal dumbbell: two 25000-px
     * clumps at ±219 px from the centroid, so μ₂₀ = 50000·219² = 2.397e9
     * and μ₀₂ is small (clumps are not tall). Without the k_shift scale,
     * tr*tr wraps and eccentricity lands outside [0, 1] in Q16.16 units. */
    tpv_Blob b = {0};
    b.m00  = 50000;
    b.m10  = 50000 * 320;
    b.m01  = 50000 * 240;
    b.mu20 = 2397020000LL;   /* ≈ 2.4e9 */
    b.mu02 = 50000LL * 4 * 4;/* small */
    b.mu11 = 0;
    b.perimeter = 500;
    b.bbox_x0 = 0; b.bbox_x1 = 639;
    b.bbox_y0 = 236; b.bbox_y1 = 244;

    tpv_Features f;
    tpv_shape_features(&b, &f);
    /* Eccentricity for a highly-elongated object approaches 1.0 (Q16.16 = 65536).
     * Must land in [0, 1.01·65536] — absolutely not negative (overflow sentinel)
     * and absolutely not > 2·65536 (another overflow sign). */
    CHECK(f.eccentricity >= 0);
    CHECK(f.eccentricity <= (int32_t)(1.01 * 65536));
    /* And should be close to 1 for this very elongated shape. */
    CHECK(f.eccentricity > (int32_t)(0.99 * 65536));
}

int main(void) {
    RUN(t_square_features_symmetric);
    RUN(t_large_blob_no_eccentricity_overflow);
    FINISH();
}
