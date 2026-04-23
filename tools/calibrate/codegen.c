#include <stdio.h>
#include "tpv_internal.h"

/* Emit a C source file that defines `tpv_bin_threshold` and the
 * `tpv_templates[]` array with the calibrated values, ready to be compiled
 * into the embedded firmware. The runtime extern declarations in
 * tpv_internal.h pick this up at link time. */
int tpv_cal_emit_model(const tpv_Template *tmpl, int n,
                       uint8_t bin_thresh, FILE *out) {
    fprintf(out, "/* AUTO-GENERATED — do not hand-edit */\n");
    fprintf(out, "#include \"tpv_internal.h\"\n\n");
    fprintf(out, "const uint8_t tpv_bin_threshold = %u;\n\n", bin_thresh);
    fprintf(out, "const tpv_Template tpv_templates[%d] = {\n", n);
    for (int c = 0; c < n; c++) {
        fprintf(out, "    {\n        .mean = {\n            ");
        const int32_t *m = (const int32_t *)&tmpl[c].mean;
        for (int i = 0; i < TPV_N_FEAT; i++) fprintf(out, "0x%08x, ", (uint32_t)m[i]);
        fprintf(out, "\n        },\n        .L_inv = {\n            ");
        for (int i = 0; i < TPV_L_INV_N; i++) {
            fprintf(out, "0x%08x, ", (uint32_t)tmpl[c].L_inv[i]);
            if ((i + 1) % 8 == 0) fprintf(out, "\n            ");
        }
        fprintf(out, "\n        },\n");
        fprintf(out, "        .reject_thresh = 0x%08x,\n", (uint32_t)tmpl[c].reject_thresh);
        fprintf(out, "        .margin        = 0x%08x,\n    },\n", (uint32_t)tmpl[c].margin);
    }
    fprintf(out, "};\n");
    return 0;
}
