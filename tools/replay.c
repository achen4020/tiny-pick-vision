/* Host-side regression tool: walk a directory of raw 640×480 Y frames,
 * run tpv_process_frame (v1) or tpv_process_frame_debug_v2 (v2) on each,
 * emit one CSV row per frame.
 *
 * Mode auto-selection: read <run_dir>/meta.json. If it declares
 * ui_version == "v2", run v2 with the meta's tpv.bin_threshold /
 * dark_object_mode / roi as algorithm inputs. Otherwise run v1.
 * CLI flags --v1 / --v2 / --bin-threshold / --dark-object-mode / --roi
 * override the meta-derived defaults for diagnostic use.
 *
 * Usage:    replay [--v1|--v2] [--bin-threshold N] [--dark-object-mode 0|1]
 *                  [--roi x,y,w,h] <run_dir_or_frames_dir>  > release.csv
 * Compare:  diff baseline.csv release.csv     # joins by frame_name (column 0)
 *
 * Sorted alphabetically so re-runs and runs on different hosts produce
 * byte-identical output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <string.h>
#include "tpv.h"
#include "tpv_internal.h"

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

typedef struct {
    int is_v2;               /* 1 if meta declares ui_version == "v2" */
    uint8_t bin_threshold;   /* from meta.json or default 128 */
    int dark_object_mode;    /* 0 or 1 */
    int roi_x, roi_y, roi_w, roi_h;
} ReplayMeta;

/* Best-effort field lookup: finds the first occurrence of "key" : <value>
 * and parses. Not robust against escaped strings, but our meta.json is
 * produced by JSONObject.toString(2) with predictable formatting. */
static int meta_find_int(const char *json, const char *key, int *out) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p && (*p == ' ' || *p == '\n' || *p == '\t')) p++;
    if (*p == 't') { *out = 1; return 1; }  /* true */
    if (*p == 'f') { *out = 0; return 1; }  /* false */
    *out = atoi(p);
    return 1;
}

static int meta_find_string(const char *json, const char *key,
                             char *out, size_t out_size) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p && (*p == ' ' || *p == '\n' || *p == '\t')) p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size) out[n++] = *p++;
    out[n] = '\0';
    return 1;
}

static int read_meta(const char *run_dir, ReplayMeta *m) {
    /* Defaults — v1 mode / full frame / default threshold */
    m->is_v2 = 0;
    m->bin_threshold = 128;
    m->dark_object_mode = 0;
    m->roi_x = 0; m->roi_y = 0; m->roi_w = 640; m->roi_h = 480;

    char path[1024];
    snprintf(path, sizeof path, "%s/meta.json", run_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;   /* no meta.json → v1 mode with defaults */
    char buf[16384] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    char ver[32] = {0};
    if (meta_find_string(buf, "ui_version", ver, sizeof ver) &&
        strcmp(ver, "v2") == 0) m->is_v2 = 1;

    int tmp;
    if (m->is_v2) {
        if (meta_find_int(buf, "bin_threshold", &tmp)) m->bin_threshold = (uint8_t)tmp;
        if (meta_find_int(buf, "dark_object_mode", &tmp)) m->dark_object_mode = tmp;
        /* meta.json key order is device → tpv → trigger → camera; no "x/y/w/h"
         * appears in the device block, so the first match for these short
         * keys lands on tpv.roi.{x,y,w,h}. See RunRecorder.kt:metaToJson. */
        if (meta_find_int(buf, "x", &tmp)) m->roi_x = tmp;
        if (meta_find_int(buf, "y", &tmp)) m->roi_y = tmp;
        if (meta_find_int(buf, "w", &tmp)) m->roi_w = tmp;
        if (meta_find_int(buf, "h", &tmp)) m->roi_h = tmp;
    }
    return 1;
}

static int parse_roi(const char *s, int *x, int *y, int *w, int *h) {
    return sscanf(s, "%d,%d,%d,%d", x, y, w, h) == 4;
}

int main(int argc, char **argv) {
    const char *run_dir = NULL;
    int force_v1 = 0, force_v2 = 0;
    int override_thr = -1, override_dark = -1;
    int override_roi_x = -1, roi_y = 0, roi_w = 0, roi_h = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--v1")) force_v1 = 1;
        else if (!strcmp(argv[i], "--v2")) force_v2 = 1;
        else if (!strcmp(argv[i], "--bin-threshold") && i+1 < argc) {
            override_thr = atoi(argv[++i]);
            if (override_thr < 0 || override_thr > 255) {
                fprintf(stderr, "--bin-threshold must be 0..255\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--dark-object-mode") && i+1 < argc) {
            override_dark = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--roi") && i+1 < argc) {
            if (!parse_roi(argv[++i], &override_roi_x, &roi_y, &roi_w, &roi_h)) {
                fprintf(stderr, "--roi needs x,y,w,h\n"); return 2;
            }
        } else if (argv[i][0] != '-') {
            run_dir = argv[i];
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2;
        }
    }
    if (!run_dir) {
        fprintf(stderr, "usage: replay [--v1|--v2] [--bin-threshold N] [--dark-object-mode 0|1] [--roi x,y,w,h] <run_dir_or_frames_dir>\n");
        return 2;
    }

    ReplayMeta meta;
    read_meta(run_dir, &meta);
    if (force_v1) meta.is_v2 = 0;
    if (force_v2) meta.is_v2 = 1;
    if (override_thr >= 0)  meta.bin_threshold = (uint8_t)override_thr;
    if (override_dark >= 0) meta.dark_object_mode = override_dark;
    if (override_roi_x >= 0) {
        meta.roi_x = override_roi_x; meta.roi_y = roi_y;
        meta.roi_w = roi_w; meta.roi_h = roi_h;
    }

    DIR *dir = opendir(run_dir);
    if (!dir) { perror("opendir"); return 1; }

    /* Slurp every entry into a heap-allocated array, then qsort. Any fixed
     * upper bound would silently drop frames past the cap in readdir() order
     * (filesystem-defined), which would make release.csv drift across hosts
     * despite the sort — the same reproducibility leak we already fixed in
     * tools/calibrate/frame_io.c. Grow geometrically; no cap. */
    char **names = NULL;
    size_t cap = 0, nn = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (nn == cap) {
            size_t new_cap = cap ? cap * 2 : 64;
            char **grown = realloc(names, new_cap * sizeof *grown);
            if (!grown) {
                for (size_t k = 0; k < nn; k++) free(names[k]);
                free(names);
                closedir(dir);
                fprintf(stderr, "replay: out of memory growing name list\n");
                return 1;
            }
            names = grown;
            cap = new_cap;
        }
        char *dup = strdup(e->d_name);
        if (!dup) {
            for (size_t k = 0; k < nn; k++) free(names[k]);
            free(names);
            closedir(dir);
            fprintf(stderr, "replay: out of memory duplicating %s\n", e->d_name);
            return 1;
        }
        names[nn++] = dup;
    }
    closedir(dir);
    qsort(names, nn, sizeof names[0], by_name);

    printf("frame_name,status,class_id,x,y,theta_x10,confidence,bbox_area_px,grid_8x8\n");
    static uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    for (size_t i = 0; i < nn; i++) {
        /* Only process files ending in .y — run dirs also contain meta.json,
         * events.jsonl, *.jpg, *.mask, etc. */
        size_t nlen = strlen(names[i]);
        if (nlen < 2 || strcmp(names[i] + nlen - 2, ".y") != 0) {
            free(names[i]);
            continue;
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", run_dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { free(names[i]); continue; }
        if (fread(y, 1, sizeof y, f) == sizeof y) {
            if (meta.is_v2) {
                tpv_DetectionDebugV2 d;
                int rc = tpv_process_frame_debug_v2(
                    y, TPV_WIDTH, TPV_HEIGHT,
                    meta.bin_threshold, meta.dark_object_mode,
                    meta.roi_x, meta.roi_y, meta.roi_w, meta.roi_h,
                    &d);
                printf("%s,%d,%u,%d,%d,%d,%u,%d,%d\n",
                    names[i], rc, d.det.class_id, d.det.x, d.det.y,
                    d.det.theta_x10, d.det.confidence_q8,
                    d.area_px, d.grid_8x8);
            } else {
                tpv_Detection dv1;
                int rc = tpv_process_frame(y, TPV_WIDTH, TPV_HEIGHT, &dv1);
                printf("%s,%d,%u,%d,%d,%d,%u,,\n",
                    names[i], rc, dv1.class_id, dv1.x, dv1.y,
                    dv1.theta_x10, dv1.confidence_q8);
            }
        }
        fclose(f);
        free(names[i]);
    }
    free(names);
    return 0;
}
