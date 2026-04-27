#ifndef TPV_VISION_POLICY_H
#define TPV_VISION_POLICY_H

#include <stdint.h>
#include "tpv_vision.h"

int tpv_vision_policy_is_primary_engine(const tpv_vision_config *cfg,
                                        uint32_t engine_id);
void tpv_vision_policy_apply(const tpv_vision_config *cfg,
                             tpv_vision_detection *detections,
                             int detection_count);

#endif
