#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "tpv_internal.h"

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
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
