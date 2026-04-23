#include <string.h>
#include "tpv_internal.h"

/* Working buffers (spec §6); bitmap is passed in as a parameter,
 * not stored here to avoid -Werror -Wunused-variable issues. */
static uint16_t g_labels[TPV_WIDTH * TPV_HEIGHT];
static uint32_t g_uf[TPV_MAX_LABELS + 1];

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
    /* Pass 1: scan + 4-neighbour (up & left), assign temporary labels */
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

    /* Build remap table: resolve equivalence classes to compact indices 1..n_blobs */
    static uint16_t remap[TPV_MAX_LABELS + 1];
    memset(remap, 0, next_label * sizeof(uint16_t));
    int n_blobs = 0;
    for (uint32_t l = 1; l < next_label; l++) {
        uint32_t r = uf_find(l);
        if (r == l) {
            if (n_blobs >= max_blobs) return -2;  /* SCENE_ERROR */
            remap[l] = (uint16_t)(n_blobs + 1);
            /* Zero-init this blob and set initial bbox bounds */
            tpv_Blob *b = &blobs_out[n_blobs];
            memset(b, 0, sizeof *b);
            b->bbox_x0 = (int16_t)TPV_WIDTH;
            b->bbox_y0 = (int16_t)TPV_HEIGHT;
            b->bbox_x1 = -1;
            b->bbox_y1 = -1;
            n_blobs++;
        }
    }
    /* Second loop: non-root labels inherit their root's compact index */
    for (uint32_t l = 1; l < next_label; l++) {
        uint32_t r = uf_find(l);
        remap[l] = remap[r];
    }

    /* Pass 2: accumulate m00, m10, m01, perimeter (per-edge), bbox */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            uint16_t rl = g_labels[idx] ? remap[g_labels[idx]] : 0;
            if (!rl) continue;
            tpv_Blob *b = &blobs_out[rl - 1];
            b->m00 += 1;
            b->m10 += x;
            b->m01 += y;
            if (x < b->bbox_x0) b->bbox_x0 = (int16_t)x;
            if (y < b->bbox_y0) b->bbox_y0 = (int16_t)y;
            if (x > b->bbox_x1) b->bbox_x1 = (int16_t)x;
            if (y > b->bbox_y1) b->bbox_y1 = (int16_t)y;
            /* Perimeter: per-edge count — each background-adjacent edge contributes 1 */
            int contrib = 0;
            if (!bit_at(bin, w, h, x - 1, y)) contrib++;
            if (!bit_at(bin, w, h, x + 1, y)) contrib++;
            if (!bit_at(bin, w, h, x,     y - 1)) contrib++;
            if (!bit_at(bin, w, h, x,     y + 1)) contrib++;
            b->perimeter += contrib;
        }
    }

    /* Pass 3: per blob, compute integer centroid, then accumulate central moments */
    for (int i = 0; i < n_blobs; i++) {
        tpv_Blob *b = &blobs_out[i];
        if (b->m00 == 0) continue;
        int32_t cx_q0 = b->m10 / b->m00;
        int32_t cy_q0 = b->m01 / b->m00;
        for (int y = b->bbox_y0; y <= b->bbox_y1; y++) {
            for (int x = b->bbox_x0; x <= b->bbox_x1; x++) {
                int idx = y * w + x;
                uint16_t rl = g_labels[idx] ? remap[g_labels[idx]] : 0;
                if (rl != (uint16_t)(i + 1)) continue;
                int64_t dx = x - cx_q0;
                int64_t dy = y - cy_q0;
                b->mu20 += dx * dx;
                b->mu11 += dx * dy;
                b->mu02 += dy * dy;
                b->mu30 += dx * dx * dx;
                b->mu21 += dx * dx * dy;
                b->mu12 += dx * dy * dy;
                b->mu03 += dy * dy * dy;
            }
        }
    }
    return n_blobs;
}
