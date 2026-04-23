#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "tpv_internal.h"

/* Mutable storage in cal_stub.c — calibration sets this before sample loading
 * so tpv_threshold() (linked from src/) uses the calibrated value. */
extern uint8_t tpv_bin_threshold;

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* Otsu's method: build a histogram from up to 5 frames per directory and pick
 * the threshold maximizing between-class variance. Returns the chosen value
 * AND writes it into the runtime extern so subsequent tpv_threshold() calls
 * (in tpv_cal_load_class_frames) see it. Falls back to TPV_BIN_THRESH_DEFAULT
 * if no readable frames. Doubles are fine: this is a one-shot host-side
 * computation, not the runtime per-frame path. */
uint8_t tpv_cal_estimate_bin_threshold(const char *const *dirs, int n_dirs) {
    static uint64_t hist[256];
    memset(hist, 0, sizeof hist);
    static uint8_t y[TPV_WIDTH * TPV_HEIGHT];

    for (int d = 0; d < n_dirs; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        int n_loaded = 0;
        while ((e = readdir(dir)) && n_loaded < 5) {
            if (e->d_name[0] == '.') continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", dirs[d], e->d_name);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            if (fread(y, 1, sizeof y, f) == sizeof y) {
                for (int i = 0; i < TPV_WIDTH * TPV_HEIGHT; i++) hist[y[i]]++;
                n_loaded++;
            }
            fclose(f);
        }
        closedir(dir);
    }

    uint64_t total = 0, sum_total = 0;
    for (int i = 0; i < 256; i++) {
        total     += hist[i];
        sum_total += (uint64_t)i * hist[i];
    }
    if (total == 0) {
        tpv_bin_threshold = TPV_BIN_THRESH_DEFAULT;
        return tpv_bin_threshold;
    }

    uint64_t w0 = 0, sum0 = 0;
    double max_var = -1.0;
    int best_t = TPV_BIN_THRESH_DEFAULT;
    for (int t = 0; t < 256; t++) {
        w0   += hist[t];
        sum0 += (uint64_t)t * hist[t];
        if (w0 == 0) continue;
        uint64_t w1 = total - w0;
        if (w1 == 0) break;
        double m0  = (double)sum0 / (double)w0;
        double m1  = (double)(sum_total - sum0) / (double)w1;
        double var = (double)w0 * (double)w1 * (m0 - m1) * (m0 - m1);
        if (var > max_var) { max_var = var; best_t = t; }
    }

    tpv_bin_threshold = (uint8_t)best_t;
    return tpv_bin_threshold;
}

/* Walk a directory of raw 640×480 Y frames (one per file), extract the
 * single calibration blob from each, and pack TPV_Features into out[].
 * Sorted alphabetically so calibration results are reproducible across runs. */
int tpv_cal_load_class_frames(const char *dir, tpv_Features *out, int cap) {
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return 0; }

    char *names[1024];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < (int)(sizeof names / sizeof names[0])) {
        if (e->d_name[0] == '.') continue;
        names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof names[0], by_name);

    static uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    static uint8_t bin[TPV_WIDTH * TPV_HEIGHT / 8];
    static tpv_Blob blobs[TPV_MAX_BLOBS];
    int n = 0;
    for (int i = 0; i < nn && n < cap; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { free(names[i]); continue; }
        if (fread(y, 1, sizeof y, f) == sizeof y) {
            tpv_threshold(y, TPV_WIDTH, TPV_HEIGHT, bin);
            int nb = tpv_ccl_moments(bin, TPV_WIDTH, TPV_HEIGHT, blobs, TPV_MAX_BLOBS);
            /* Calibration scenes should have exactly one in-range blob. */
            if (nb == 1 && blobs[0].m00 >= TPV_AMIN && blobs[0].m00 <= TPV_AMAX) {
                tpv_shape_features(&blobs[0], &out[n++]);
            }
        }
        fclose(f);
        free(names[i]);
    }
    return n;
}
