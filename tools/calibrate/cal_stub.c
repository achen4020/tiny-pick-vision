#include "tpv_internal.h"

/* During calibration the binary threshold default is good enough — the runtime
 * model_data.c emitted by the tool will carry the actual calibrated value.
 * tpv_threshold() (linked from src/) reads this extern, so we must define it. */
const uint8_t tpv_bin_threshold = TPV_BIN_THRESH_DEFAULT;

/* tpv_templates is referenced by extern in tpv_internal.h; calibration never
 * reads it, but the linker still needs a definition to resolve the symbol
 * coming from src/ object files we link in. */
#if TPV_N_CLASSES >= 1
const tpv_Template tpv_templates[TPV_N_CLASSES] = {0};
#endif
