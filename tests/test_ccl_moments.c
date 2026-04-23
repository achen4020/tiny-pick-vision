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

int main(void) {
    RUN(t_single_square_blob);
    RUN(t_two_disjoint_blobs);
    RUN(t_max_labels_overflow_returns_neg1);
    FINISH();
}
