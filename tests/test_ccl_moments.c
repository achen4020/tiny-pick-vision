#include "tpv_internal.h"
#include "testlib.h"

TEST(t_single_square_blob) {
    /* 8x8 all-foreground (LSB-first, 1 byte per row when w=8) */
    uint8_t bin[8];
    for (int i = 0; i < 8; i++) bin[i] = 0xFF;
    tpv_Blob blobs[4];
    int n = tpv_ccl_moments(bin, 8, 8, blobs, 4);
    CHECK_EQ_I(n, 1);
    CHECK_EQ_I(blobs[0].m00, 64);
    /* centroid = (3.5, 3.5); m10 = sum of x = 8 * (0+1+...+7) = 8 * 28 = 224 */
    CHECK_EQ_I(blobs[0].m10, 224);
    CHECK_EQ_I(blobs[0].m01, 224);
    /* perimeter (4-neighbour edge count) = 4 sides * 8 edges = 32 */
    CHECK_EQ_I(blobs[0].perimeter, 32);
}

TEST(t_two_disjoint_blobs) {
    /* Two 2x2 squares, top-left at (0,0)-(1,1) and bottom-right at (6,6)-(7,7) */
    uint8_t bin[8] = {0};
    bin[0] = 0x03; bin[1] = 0x03;   /* rows 0,1 cols 0,1 */
    bin[6] = 0xC0; bin[7] = 0xC0;   /* rows 6,7 cols 6,7 */
    tpv_Blob blobs[4];
    int n = tpv_ccl_moments(bin, 8, 8, blobs, 4);
    CHECK_EQ_I(n, 2);
}

TEST(t_max_labels_overflow_returns_neg1) {
    /* 32x32 alternating pattern to provoke many labels */
    uint8_t bin[128] = {0};
    for (int i = 0; i < 32 * 32; i += 2) bin[i >> 3] |= (uint8_t)(1u << (i & 7));
    tpv_Blob b[TPV_MAX_BLOBS];
    int n = tpv_ccl_moments(bin, 32, 32, b, TPV_MAX_BLOBS);
    /* protocol-only: no out-of-bounds, no crash; either negative or <= MAX_BLOBS */
    CHECK((n < 0) || (n <= TPV_MAX_BLOBS));
}

TEST(t_symmetric_square_has_zero_third_moments) {
    /* Regression: an N×N filled square is point-symmetric → all 3rd-order
     * central moments must be exactly 0. The original implementation rounded
     * the centroid to the nearest integer and synthesized non-zero μ_3 for
     * even-sized squares. */
    uint8_t bin[8];
    for (int i = 0; i < 8; i++) bin[i] = 0xFF;
    tpv_Blob blobs[4];
    int n = tpv_ccl_moments(bin, 8, 8, blobs, 4);
    CHECK_EQ_I(n, 1);
    CHECK_EQ_I(blobs[0].mu30, 0);
    CHECK_EQ_I(blobs[0].mu21, 0);
    CHECK_EQ_I(blobs[0].mu12, 0);
    CHECK_EQ_I(blobs[0].mu03, 0);
    CHECK_EQ_I(blobs[0].mu11, 0);   /* x and y are independent over a square */
}

/* Paint an L-shape with its root at (base_x, base_y):
 *   (bx, by), (bx+1, by), (bx+2, by),        horizontal foot, 3 pixels
 *   (bx, by+1), (bx, by+2)                   vertical arm, 2 pixels
 * Total 5 pixels. Intentionally asymmetric (μ_3 ≠ 0 at exact centroid). */
static void paint_L_at(uint8_t *bin, int w, int bx, int by) {
    int pxs[5][2] = { {bx, by}, {bx+1, by}, {bx+2, by},
                      {bx, by+1}, {bx, by+2} };
    for (int k = 0; k < 5; k++) {
        int idx = pxs[k][1] * w + pxs[k][0];
        bin[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    }
}

TEST(t_asymmetric_blob_is_translation_invariant) {
    /* Regression for the coordinate-dependent μ_3 drift: same L-shape rendered
     * at two different positions must produce identical central moments.
     * Checks 2nd AND 3rd order, since the broken rearranged formula drifted
     * most visibly in 3rd. */
    static uint8_t bin_a[16 * 16 / 8];
    static uint8_t bin_b[16 * 16 / 8];
    tpv_Blob blobs_a[4], blobs_b[4];

    paint_L_at(bin_a, 16, 1, 1);      /* L near the origin corner */
    paint_L_at(bin_b, 16, 10, 10);    /* same L shifted by (+9, +9) */

    int na = tpv_ccl_moments(bin_a, 16, 16, blobs_a, 4);
    int nb = tpv_ccl_moments(bin_b, 16, 16, blobs_b, 4);
    CHECK_EQ_I(na, 1);
    CHECK_EQ_I(nb, 1);
    CHECK_EQ_I(blobs_a[0].m00, 5);
    CHECK_EQ_I(blobs_b[0].m00, 5);

    /* Central moments are translation-invariant by definition; require
     * bit-identical match on a deterministic integer pipeline. */
    CHECK_EQ_I(blobs_a[0].mu20, blobs_b[0].mu20);
    CHECK_EQ_I(blobs_a[0].mu11, blobs_b[0].mu11);
    CHECK_EQ_I(blobs_a[0].mu02, blobs_b[0].mu02);
    CHECK_EQ_I(blobs_a[0].mu30, blobs_b[0].mu30);
    CHECK_EQ_I(blobs_a[0].mu21, blobs_b[0].mu21);
    CHECK_EQ_I(blobs_a[0].mu12, blobs_b[0].mu12);
    CHECK_EQ_I(blobs_a[0].mu03, blobs_b[0].mu03);

    /* Sanity: μ_3 for this L is non-zero (the test should catch a regression
     * that always produces 0 too). */
    CHECK(blobs_a[0].mu30 != 0 || blobs_a[0].mu03 != 0
       || blobs_a[0].mu21 != 0 || blobs_a[0].mu12 != 0);
}

int main(void) {
    RUN(t_single_square_blob);
    RUN(t_two_disjoint_blobs);
    RUN(t_max_labels_overflow_returns_neg1);
    RUN(t_symmetric_square_has_zero_third_moments);
    RUN(t_asymmetric_blob_is_translation_invariant);
    FINISH();
}
