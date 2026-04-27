#include "vision_policy.h"

int tpv_vision_policy_is_primary_engine(const tpv_vision_config *cfg,
                                        uint32_t engine_id) {
    return cfg && engine_id == cfg->primary_event_engine;
}

void tpv_vision_policy_apply(const tpv_vision_config *cfg,
                             tpv_vision_detection *detections,
                             int detection_count) {
    if (!cfg || !detections || detection_count <= 0) return;

    for (int i = 0; i < detection_count; i++) {
        detections[i].flags &= (uint16_t)~TPV_DETECTION_PRIMARY;
        if (tpv_vision_policy_is_primary_engine(cfg, detections[i].engine_id)) {
            detections[i].flags |= TPV_DETECTION_PRIMARY;
        }
    }
}
