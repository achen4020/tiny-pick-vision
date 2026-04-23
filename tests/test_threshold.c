#include <string.h>
#include "tpv_internal.h"
#include "testlib.h"

extern const uint8_t tpv_bin_threshold;  /* from stub_model_data.c */

TEST(t_all_zero_maps_to_zero_bits) {
    uint8_t y[64] = {0};
    uint8_t bin[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    tpv_threshold(y, 64, 1, bin);
    for (int i = 0; i < 8; i++) CHECK_EQ_I(bin[i], 0x00);
}

TEST(t_all_255_maps_to_all_ones) {
    uint8_t y[64];
    memset(y, 255, sizeof y);
    uint8_t bin[8] = {0};
    tpv_threshold(y, 64, 1, bin);
    for (int i = 0; i < 8; i++) CHECK_EQ_I(bin[i], 0xFF);
}

TEST(t_at_threshold_is_foreground) {
    /* Convention: Y >= threshold ⇒ 1 (foreground).
     * Default threshold from stub_model_data.c is 128.
     * Y values: {0, 127, 128, 129, 200, 127, 255, 0}.
     * Expected: indices 2,3,4,6 are foreground (>=128).
     * LSB-first packing: bin[0] = (1<<2)|(1<<3)|(1<<4)|(1<<6) = 0x5C. */
    uint8_t y[8] = {0, 127, 128, 129, 200, 127, 255, 0};
    uint8_t bin[1] = {0};
    tpv_threshold(y, 8, 1, bin);
    CHECK_EQ_I(bin[0], 0x5C);  /* 0101 1100 */
}

TEST(t_non_byte_aligned_npix) {
    /* w*h not a multiple of 8: only the lower bits of bin[0] are written.
     * Y values: {200, 0, 200, 0, 200} with default threshold 128.
     * Foreground at indices 0, 2, 4 → bin[0] = (1<<0)|(1<<2)|(1<<4) = 0x15.
     * Bits 5..7 must remain 0 (initialized by the function's clear loop). */
    uint8_t y[5] = {200, 0, 200, 0, 200};
    uint8_t bin[1] = {0xFF};   /* poison: function must clear before writing */
    tpv_threshold(y, 5, 1, bin);
    CHECK_EQ_I(bin[0], 0x15);
}

int main(void) {
    RUN(t_all_zero_maps_to_zero_bits);
    RUN(t_all_255_maps_to_all_ones);
    RUN(t_at_threshold_is_foreground);
    RUN(t_non_byte_aligned_npix);
    FINISH();
}
