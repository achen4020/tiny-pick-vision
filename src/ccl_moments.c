#include <string.h>
#include "tpv_internal.h"

/* Working buffers (spec §6); bitmap is passed in as a parameter, not stored
 * here to avoid -Werror -Wunused-variable issues. */
static uint16_t g_labels[TPV_WIDTH * TPV_HEIGHT];
static uint32_t g_uf[TPV_MAX_LABELS + 1];
static uint16_t g_remap[TPV_MAX_LABELS + 1];

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

    /* Build remap table: resolve equivalence classes to compact 1..n_blobs. */
    memset(g_remap, 0, next_label * sizeof(uint16_t));
    int n_blobs = 0;
    for (uint32_t l = 1; l < next_label; l++) {
        uint32_t r = uf_find(l);
        if (r == l) {
            if (n_blobs >= max_blobs) return -2;  /* SCENE_ERROR */
            g_remap[l] = (uint16_t)(n_blobs + 1);
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
        g_remap[l] = g_remap[r];
    }

    /* Pass 2: per pixel, accumulate m00, m10, m01, perimeter, bbox.
     * Raw 2nd/3rd moments are *not* accumulated here — they'd be straightforward
     * but the analytical raw→central conversion overflows int64 on 32-bit ARM
     * (no __int128) *and* truncated integer division in a rearranged formula
     * breaks translation invariance (an L-shape's μ₃ drifts with x-position,
     * which feeds bogus per-position features into Hu invariants). */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            uint16_t rl = g_labels[idx] ? g_remap[g_labels[idx]] : 0;
            if (!rl) continue;
            tpv_Blob *b = &blobs_out[rl - 1];

            b->m00 += 1;
            b->m10 += x;
            b->m01 += y;

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

    /* Pass 3: per blob, compute Q16.16 centroid, then iterate its bbox to
     * accumulate central moments from exact fractional offsets.
     *
     * Why Q16.16 cx rather than integer cx: an integer cx = ⌊sx/M⌋ loses the
     * fractional centroid (e.g. 3.5 for an 8×8 square → 3), which synthesizes
     * non-zero μ₃ for symmetric blobs. Q16.16 cx_q16 = ⌊sx·2¹⁶/M⌋ keeps the
     * fractional part accurate to 1/2¹⁶. Crucially, translation invariance is
     * preserved exactly: under x ← x + Δ, cx_q16 increases by Δ·2¹⁶ (exactly,
     * because ΔM·2¹⁶ divides M), so dx = (x·2¹⁶ − cx_q16) is coordinate-free.
     *
     * Magnitudes on the deployment target (m00 ≤ TPV_AMAX = 50000, W ≤ 640):
     *   |dx|  ≤ W·2¹⁶ ≈ 4.2e7                   (int64 ✓)
     *   dx²  ≤ 1.8e15, >>16 into Q16.16 ≤ 2.8e10 per pixel
     *   Σ over 50000 px: ≤ 1.4e15 (int64 ✓)
     *   dx³  (Q16.16) per pixel ≤ 2.8e10·4.2e7>>16 ≈ 1.8e13
     *   Σ over 50000 px: ≤ 9e17 (int64 ✓, margin vs 9.2e18 max). */
    for (int i = 0; i < n_blobs; i++) {
        tpv_Blob *b = &blobs_out[i];
        int64_t M = b->m00;
        if (M == 0 || M > TPV_AMAX) continue;
        int64_t cx_q16 = (b->m10 * (int64_t)65536) / M;
        int64_t cy_q16 = (b->m01 * (int64_t)65536) / M;

        int64_t a20 = 0, a11 = 0, a02 = 0;
        int64_t a30 = 0, a21 = 0, a12 = 0, a03 = 0;
        for (int y = b->bbox_y0; y <= b->bbox_y1; y++) {
            for (int x = b->bbox_x0; x <= b->bbox_x1; x++) {
                int idx = y * w + x;
                uint16_t rl = g_labels[idx] ? g_remap[g_labels[idx]] : 0;
                if (rl != (uint16_t)(i + 1)) continue;

                int64_t dx = ((int64_t)x << 16) - cx_q16;
                int64_t dy = ((int64_t)y << 16) - cy_q16;
                int64_t dx2 = (dx * dx) >> 16;           /* Q16.16 */
                int64_t dy2 = (dy * dy) >> 16;
                int64_t dxy = (dx * dy) >> 16;

                a20 += dx2;
                a11 += dxy;
                a02 += dy2;
                a30 += (dx2 * dx) >> 16;                 /* Q16.16 */
                a21 += (dx2 * dy) >> 16;
                a12 += (dxy * dy) >> 16;
                a03 += (dy2 * dy) >> 16;
            }
        }
        /* >> 16 shift converts Q16.16 accumulators to integer μ_pq. */
        b->mu20 = a20 >> 16;
        b->mu11 = a11 >> 16;
        b->mu02 = a02 >> 16;
        b->mu30 = a30 >> 16;
        b->mu21 = a21 >> 16;
        b->mu12 = a12 >> 16;
        b->mu03 = a03 >> 16;
    }

    return n_blobs;
}
