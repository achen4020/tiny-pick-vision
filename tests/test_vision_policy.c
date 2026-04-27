#include <string.h>
#include "tpv_vision.h"
#include "testlib.h"
#include "../src/vision_policy.h"

static tpv_vision_detection detection(uint32_t engine_id, uint16_t flags) {
    tpv_vision_detection d;
    memset(&d, 0, sizeof d);
    d.engine_id = engine_id;
    d.flags = flags;
    return d;
}

TEST(t_primary_policy_marks_only_primary_engine) {
    tpv_vision_config cfg;
    tpv_vision_default_config(&cfg);
    cfg.primary_event_engine = TPV_ENGINE_ID_FACE;

    tpv_vision_detection detections[3];
    detections[0] = detection(TPV_ENGINE_ID_TPV_BLOB,
                              TPV_DETECTION_PRIMARY | TPV_DETECTION_HAS_CENTER);
    detections[1] = detection(TPV_ENGINE_ID_FACE, TPV_DETECTION_HAS_BBOX);
    detections[2] = detection(TPV_ENGINE_ID_OBJECT, TPV_DETECTION_PRIMARY);

    tpv_vision_policy_apply(&cfg, detections, 3);

    CHECK((detections[0].flags & TPV_DETECTION_PRIMARY) == 0);
    CHECK(detections[0].flags & TPV_DETECTION_HAS_CENTER);
    CHECK(detections[1].flags & TPV_DETECTION_PRIMARY);
    CHECK(detections[1].flags & TPV_DETECTION_HAS_BBOX);
    CHECK((detections[2].flags & TPV_DETECTION_PRIMARY) == 0);
}

TEST(t_primary_engine_helper_is_numeric_and_strict) {
    tpv_vision_config cfg;
    tpv_vision_default_config(&cfg);
    cfg.primary_event_engine = TPV_ENGINE_ID_TPV_BLOB;

    CHECK_EQ_I(tpv_vision_policy_is_primary_engine(&cfg, TPV_ENGINE_ID_TPV_BLOB), 1);
    CHECK_EQ_I(tpv_vision_policy_is_primary_engine(&cfg, TPV_ENGINE_ID_FACE), 0);
    CHECK_EQ_I(tpv_vision_policy_is_primary_engine(0, TPV_ENGINE_ID_TPV_BLOB), 0);
}

TEST(t_policy_accepts_empty_detection_list) {
    tpv_vision_config cfg;
    tpv_vision_default_config(&cfg);

    tpv_vision_policy_apply(&cfg, 0, 0);
}

int main(void) {
    RUN(t_primary_policy_marks_only_primary_engine);
    RUN(t_primary_engine_helper_is_numeric_and_strict);
    RUN(t_policy_accepts_empty_detection_list);
    FINISH();
}
