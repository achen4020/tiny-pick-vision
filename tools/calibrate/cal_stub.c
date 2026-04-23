#include "tpv_internal.h"

/* In calibration the binary threshold is *estimated* (Otsu on training-frame
 * histogram, see frame_io.c) and then written back into this global before
 * features are extracted. The runtime header still declares
 * `extern const uint8_t tpv_bin_threshold` — that's fine: the runtime const
 * promise applies to the embedded build (where model_data.c's value really is
 * const). Calibration's mutable definition shadows it just for the calibrate
 * binary's link unit. */
uint8_t tpv_bin_threshold = TPV_BIN_THRESH_DEFAULT;

/* tpv_templates is referenced by extern in tpv_internal.h; calibration never
 * reads it, but the linker still needs a definition to resolve the symbol
 * coming from src/ object files we link in. */
#if TPV_N_CLASSES >= 1
const tpv_Template tpv_templates[TPV_N_CLASSES] = {0};
#endif
