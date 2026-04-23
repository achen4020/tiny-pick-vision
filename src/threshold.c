#include "tpv_internal.h"

void tpv_threshold(const uint8_t *y, int w, int h, uint8_t *bin_out) {
    const uint8_t t = tpv_bin_threshold;
    const int npix = w * h;
    const int nby  = (npix + 7) / 8;
    for (int i = 0; i < nby; i++) bin_out[i] = 0;
    /* LSB-first: bit index i corresponds to byte i/8, bit i%8 */
    for (int i = 0; i < npix; i++) {
        if (y[i] >= t) bin_out[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}
