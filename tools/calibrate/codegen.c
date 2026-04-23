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

    /* Emit a compile-time self-check: the runtime is compiled with a
     * TPV_N_CLASSES setting (default 5) that MUST match the number of
     * classes this model was calibrated for. Without this assertion,
     * `tpv_templates[n]` here would disagree with the header's
     * `extern const tpv_Template tpv_templates[TPV_N_CLASSES]` and either
     * fail to link or produce a silent size-mismatch (the extern says 5
     * entries but only n are provided — runtime could read past the end
     * of the provided array).
     *
     * This assertion fires at the earliest possible moment (building
     * model_data.c itself) with a message pointing the integrator at the
     * exact compile flag they need. */
    fprintf(out,
        "_Static_assert(TPV_N_CLASSES == %d,\n"
        "    \"Runtime TPV_N_CLASSES does not match this calibrated model.\\n\"\n"
        "    \"This model was calibrated for %d classes. Rebuild the runtime with\\n\"\n"
        "    \"    -DTPV_N_CLASSES=%d\\n\"\n"
        "    \"or recalibrate to match the currently-configured TPV_N_CLASSES.\");\n\n",
        n, n, n);

    fprintf(out, "const uint8_t tpv_bin_threshold = %u;\n\n", bin_thresh);

    /* Use the macro for the array size so the definition and the header's
     * extern declaration are literally the same expression. */
    fprintf(out, "const tpv_Template tpv_templates[TPV_N_CLASSES] = {\n");
    for (int c = 0; c < n; c++) {
        /* Use designated initializers so the emitted file compiles clean
         * under -Wall -Wextra -Wpedantic (in particular, -Wmissing-braces,
         * which flags a flat list trying to fill a struct whose first
         * field is a nested array). */
        fprintf(out, "    {\n");
        fprintf(out, "        .mean = {\n");
        fprintf(out, "            .hu = { ");
        for (int i = 0; i < 7; i++) fprintf(out, "0x%08x, ", (uint32_t)tmpl[c].mean.hu[i]);
        fprintf(out, "},\n");
        fprintf(out, "            .perim_ratio  = 0x%08x,\n", (uint32_t)tmpl[c].mean.perim_ratio);
        fprintf(out, "            .eccentricity = 0x%08x,\n", (uint32_t)tmpl[c].mean.eccentricity);
        fprintf(out, "            .m3_axis_sign = 0x%08x,\n", (uint32_t)tmpl[c].mean.m3_axis_sign);
        fprintf(out, "        },\n");
        fprintf(out, "        .L_inv = {\n            ");
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
