/* Standalone layout assertion — does not include any src/. */
#include "tpv_internal.h"
_Static_assert(sizeof(tpv_Blob) == 80, "Blob layout drift");
_Static_assert(sizeof(tpv_Features) == 40, "Features layout drift");
