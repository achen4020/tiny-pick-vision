/* Host-side regression tool: walk a directory of raw 640×480 Y frames,
 * run tpv_process_frame on each, emit one CSV row per frame.
 *
 * Usage:    replay <frames_dir>  > release.csv
 * Compare:  diff baseline.csv release.csv     # joins by frame_name (column 0)
 *
 * Sorted alphabetically so re-runs and runs on different hosts produce
 * byte-identical output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include "tpv.h"
#include "tpv_internal.h"

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: replay <frames_dir>\n"); return 2; }
    DIR *dir = opendir(argv[1]);
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

    printf("frame_name,status,class_id,x,y,theta_x10,confidence\n");
    static uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    for (size_t i = 0; i < nn; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", argv[1], names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { free(names[i]); continue; }
        if (fread(y, 1, sizeof y, f) == sizeof y) {
            tpv_Detection d;
            int r = tpv_process_frame(y, TPV_WIDTH, TPV_HEIGHT, &d);
            printf("%s,%d,%u,%d,%d,%d,%u\n",
                   names[i], r, d.class_id, d.x, d.y, d.theta_x10, d.confidence_q8);
        }
        fclose(f);
        free(names[i]);
    }
    free(names);
    return 0;
}
