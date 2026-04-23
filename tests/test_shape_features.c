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

int main(void) {
    RUN(t_square_features_symmetric);
    FINISH();
}
