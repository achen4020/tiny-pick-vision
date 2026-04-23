#include <string.h>
#include "tpv_internal.h"

/* Working buffers (spec §6); bitmap is passed in as a parameter,
 * not stored here to avoid -Werror -Wunused-variable issues. */
static uint16_t g_labels[TPV_WIDTH * TPV_HEIGHT];
static uint32_t g_uf[TPV_MAX_LABELS + 1];

/* Per-blob raw moment accumulators. We compute *raw* moments in the per-pixel
 * loop and convert to central moments analytically afterwards (standard
 * change-of-origin formulas). Doing it this way:
 *   - eliminates the truncated-centroid bug (integer cx loses fractional half
 *     and synthesizes non-zero μ₃ for symmetric blobs);
 *   - merges the previous pass-2 + pass-3 into a single pass over the bitmap. */
typedef struct {
    int64_t m20, m11, m02;
    int64_t m30, m21, m12, m03;
} RawHi;
static RawHi g_raw[TPV_MAX_BLOBS];

static uint32_t uf_find(uint32_t x) {
    while (g_uf[x] != x) {
        g_uf[x] = g_uf[g_uf[x]];  /* path compression (halving) */
        x = g_uf[x];
    }
    return x;
}

static void uf_union(uint32_t a, uint32_t b) {
    uint32_t ra = uf_find(a), rb = uf_find(b);
    if (ra == rb) return;
    if (ra < rb) g_uf[rb] = ra; else g_uf[ra] = rb;
}

static void uf_reset(int n) {
    for (int i = 0; i <= n; i++) g_uf[i] = (uint32_t)i;
}

/* Returns 1 if (x,y) is foreground, 0 if background or out-of-bounds. */
static inline int bit_at(const uint8_t *bin, int w, int h, int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    int idx = y * w + x;
    return (bin[idx >> 3] >> (idx & 7)) & 1;
}

int tpv_ccl_moments(const uint8_t *bin, int w, int h,
                    tpv_Blob *blobs_out, int max_blobs) {
    /* Pass 1: scan + 4-neighbour (up & left), assign temporary labels. */
    uf_reset(0);
    uint32_t next_label = 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            if (!((bin[idx >> 3] >> (idx & 7)) & 1)) {
                g_labels[idx] = 0;
                continue;
            }
            uint16_t up   = (y > 0) ? g_labels[idx - w] : 0;
            uint16_t left = (x > 0) ? g_labels[idx - 1] : 0;
            if (!up && !left) {
                if (next_label > TPV_MAX_LABELS) return -1;  /* SCENE_ERROR */
                g_labels[idx] = (uint16_t)next_label;
                g_uf[next_label] = next_label;
                next_label++;
            } else if (up && !left) {
                g_labels[idx] = up;
            } else if (!up && left) {
                g_labels[idx] = left;
            } else {
                g_labels[idx] = (uint16_t)((up < left) ? up : left);
                uf_union(up, left);
            }
        }
    }

    /* Build remap table: resolve equivalence classes to compact indices 1..n_blobs. */
    static uint16_t remap[TPV_MAX_LABELS + 1];
    memset(remap, 0, next_label * sizeof(uint16_t));
    int n_blobs = 0;
    for (uint32_t l = 1; l < next_label; l++) {
        uint32_t r = uf_find(l);
        if (r == l) {
            if (n_blobs >= max_blobs) return -2;  /* SCENE_ERROR */
            remap[l] = (uint16_t)(n_blobs + 1);
            tpv_Blob *b = &blobs_out[n_blobs];
            memset(b, 0, sizeof *b);
            b->bbox_x0 = (int16_t)TPV_WIDTH;
            b->bbox_y0 = (int16_t)TPV_HEIGHT;
            b->bbox_x1 = -1;
            b->bbox_y1 = -1;
            n_blobs++;
        }
    }
    for (uint32_t l = 1; l < next_label; l++) {
        uint32_t r = uf_find(l);
        remap[l] = remap[r];
    }
    /* Zero high-order raw moment accumulators for the blobs we found. */
    memset(g_raw, 0, (size_t)n_blobs * sizeof g_raw[0]);

    /* Pass 2: per pixel, accumulate raw moments + perimeter + bbox.
     * (No pass 3.) */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            uint16_t rl = g_labels[idx] ? remap[g_labels[idx]] : 0;
            if (!rl) continue;
            int blob_i = rl - 1;
            tpv_Blob *b = &blobs_out[blob_i];
            RawHi  *r = &g_raw[blob_i];

            b->m00 += 1;
            b->m10 += x;
            b->m01 += y;

            int64_t xx = (int64_t)x * x;
            int64_t yy = (int64_t)y * y;
            int64_t xy = (int64_t)x * y;
            r->m20 += xx;
            r->m11 += xy;
            r->m02 += yy;
            r->m30 += xx * x;
            r->m21 += xx * y;
            r->m12 += yy * x;
            r->m03 += yy * y;

            if (x < b->bbox_x0) b->bbox_x0 = (int16_t)x;
            if (y < b->bbox_y0) b->bbox_y0 = (int16_t)y;
            if (x > b->bbox_x1) b->bbox_x1 = (int16_t)x;
            if (y > b->bbox_y1) b->bbox_y1 = (int16_t)y;

            int contrib = 0;
            if (!bit_at(bin, w, h, x - 1, y)) contrib++;
            if (!bit_at(bin, w, h, x + 1, y)) contrib++;
            if (!bit_at(bin, w, h, x,     y - 1)) contrib++;
            if (!bit_at(bin, w, h, x,     y + 1)) contrib++;
            b->perimeter += contrib;
        }
    }

    /* Convert raw moments to central moments analytically.
     *
     *   μ20 = m20 - sx²/M
     *   μ02 = m02 - sy²/M
     *   μ11 = m11 - sx·sy/M
     *   μ30 = m30 - 3·sx·m20/M + 2·sx³/M²
     *   μ21 = m21 - 2·sx·m11/M - sy·m20/M + 2·sx²·sy/M²
     *   μ12 = m12 - 2·sy·m11/M - sx·m02/M + 2·sy²·sx/M²
     *   μ03 = m03 - 3·sy·m02/M + 2·sy³/M²
     *
     * The 32-bit ARM target has no __int128, so the intermediate products
     * must stay within int64. With m00 ≤ TPV_AMAX (50000) the bounds are:
     *   sx ≤ M·W ≤ 50000·640 = 3.2e7
     *   m20 ≤ M·W² ≤ 50000·640² = 2e10
     *   sx·m20 ≤ 6.4e17, *3 ≤ 1.9e18 → fits int64 (9.2e18).
     *   sx² ≤ 1e15 → sx²/M ≤ 1e15. Then *(sx/M) ≤ ·640 = 6.4e17 → fits.
     *   sx²·sy/M² computed as ((sx·sy)/M)·(sx/M): each factor fits int64.
     *
     * Blobs with m00 > TPV_AMAX cannot satisfy these bounds, so we skip the
     * central-moment computation for them — pipeline's L2 area filter drops
     * them anyway. Their bbox / perimeter / m00..m01 are still emitted so
     * pipeline can do the filter without surprise. */
    for (int i = 0; i < n_blobs; i++) {
        tpv_Blob *b = &blobs_out[i];
        RawHi    *r = &g_raw[i];
        int64_t M  = b->m00;
        if (M == 0 || M > TPV_AMAX) continue;
        int64_t sx = b->m10;
        int64_t sy = b->m01;

        int64_t sx2_M  = sx * sx / M;       /* ≤ ~1e15/1, but practically ≤ M·W² */
        int64_t sy2_M  = sy * sy / M;
        int64_t sxsy_M = sx * sy / M;

        b->mu20 = r->m20 - sx2_M;
        b->mu02 = r->m02 - sy2_M;
        b->mu11 = r->m11 - sxsy_M;

        b->mu30 = r->m30 - 3 * sx * r->m20 / M  + 2 * sx2_M  * sx / M;
        b->mu21 = r->m21 - 2 * sx * r->m11 / M  - sy * r->m20 / M
                                                + 2 * sxsy_M * sx / M;
        b->mu12 = r->m12 - 2 * sy * r->m11 / M  - sx * r->m02 / M
                                                + 2 * sxsy_M * sy / M;
        b->mu03 = r->m03 - 3 * sy * r->m02 / M  + 2 * sy2_M  * sy / M;
    }

    return n_blobs;
}
