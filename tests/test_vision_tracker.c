#include <string.h>
#include "tpv_vision.h"
#include "testlib.h"
#include "../src/vision_tracker.h"

static tpv_vision_detection make_detection(uint32_t engine_id,
                                           uint16_t class_id,
                                           int16_t x,
                                           int16_t y) {
    tpv_vision_detection detection;
    memset(&detection, 0, sizeof detection);
    detection.engine_id = engine_id;
    detection.detection_id = 1;
    detection.class_id = class_id;
    detection.flags = TPV_DETECTION_HAS_CENTER;
    detection.confidence_q8 = 255;
    detection.center_x = x;
    detection.center_y = y;
    return detection;
}

static tpv_vision_config tracker_config(void) {
    tpv_vision_config cfg;
    tpv_vision_default_config(&cfg);
    cfg.tracker_min_hits = 2;
    cfg.tracker_max_age = 1;
    cfg.tracker_iou_threshold = 0.25f;
    cfg.tracker_center_distance_px = 40.0f;
    return cfg;
}

TEST(t_first_hit_tentative_second_hit_confirmed) {
    tpv_vision_tracker tracker;
    tpv_vision_tracker_init(&tracker);
    tpv_vision_config cfg = tracker_config();
    tpv_vision_detection detections[1];

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);

    CHECK_EQ_I(detections[0].track_id, 1);
    CHECK(detections[0].flags & TPV_DETECTION_TRACK_TENTATIVE);
    CHECK((detections[0].flags & TPV_DETECTION_TRACK_CONFIRMED) == 0);

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 110, 102);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);

    CHECK_EQ_I(detections[0].track_id, 1);
    CHECK(detections[0].flags & TPV_DETECTION_TRACK_CONFIRMED);
    CHECK((detections[0].flags & TPV_DETECTION_TRACK_TENTATIVE) == 0);
    CHECK_EQ_I(tracker.tracks[0].hits, 2);
    CHECK_EQ_I(tracker.tracks[0].misses, 0);
}

TEST(t_min_hits_one_confirms_immediately) {
    tpv_vision_tracker tracker;
    tpv_vision_tracker_init(&tracker);
    tpv_vision_config cfg = tracker_config();
    cfg.tracker_min_hits = 1;
    tpv_vision_detection detections[1];

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);

    CHECK_EQ_I(detections[0].track_id, 1);
    CHECK(detections[0].flags & TPV_DETECTION_TRACK_CONFIRMED);
    CHECK((detections[0].flags & TPV_DETECTION_TRACK_TENTATIVE) == 0);
}

TEST(t_slow_motion_keeps_same_track) {
    tpv_vision_tracker tracker;
    tpv_vision_tracker_init(&tracker);
    tpv_vision_config cfg = tracker_config();
    tpv_vision_detection detections[1];

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);
    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 130, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);
    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 160, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);

    CHECK_EQ_I(detections[0].track_id, 1);
    CHECK_EQ_I(tracker.tracks[0].hits, 3);
}

TEST(t_far_jump_creates_new_track) {
    tpv_vision_tracker tracker;
    tpv_vision_tracker_init(&tracker);
    tpv_vision_config cfg = tracker_config();
    tpv_vision_detection detections[1];

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);
    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 260, 260);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);

    CHECK_EQ_I(detections[0].track_id, 2);
    CHECK_EQ_I(tracker.tracks[0].misses, 1);
    CHECK_EQ_I(tracker.tracks[1].hits, 1);
}

TEST(t_global_greedy_matching_is_not_detection_order_dependent) {
    tpv_vision_tracker tracker;
    tpv_vision_tracker_init(&tracker);
    tpv_vision_config cfg = tracker_config();
    cfg.tracker_center_distance_px = 20.0f;
    tpv_vision_detection detections[2];

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);
    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 130, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);
    CHECK_EQ_I(detections[0].track_id, 2);

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 115, 100);
    detections[1] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 2);

    CHECK_EQ_I(detections[0].track_id, 2);
    CHECK_EQ_I(detections[1].track_id, 1);
}

TEST(t_engine_and_class_are_isolated) {
    tpv_vision_tracker tracker;
    tpv_vision_tracker_init(&tracker);
    tpv_vision_config cfg = tracker_config();
    tpv_vision_detection detections[2];

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    detections[1] = make_detection(TPV_ENGINE_ID_FACE, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 2);

    CHECK_EQ_I(detections[0].track_id, 1);
    CHECK_EQ_I(detections[1].track_id, 2);

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 105, 105);
    detections[1] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 8, 105, 105);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 2);

    CHECK_EQ_I(detections[0].track_id, 1);
    CHECK_EQ_I(detections[1].track_id, 3);
}

TEST(t_empty_frames_age_and_expire_tracks) {
    tpv_vision_tracker tracker;
    tpv_vision_tracker_init(&tracker);
    tpv_vision_config cfg = tracker_config();
    tpv_vision_detection detections[1];

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);
    CHECK_EQ_I(tracker.tracks[0].active, 1);

    tpv_vision_tracker_update(&tracker, &cfg, 0, 0);
    CHECK_EQ_I(tracker.tracks[0].active, 1);
    CHECK_EQ_I(tracker.tracks[0].misses, 1);
    CHECK(tracker.tracks[0].detection.flags & TPV_DETECTION_TRACK_LOST);

    tpv_vision_tracker_update(&tracker, &cfg, 0, 0);
    CHECK_EQ_I(tracker.tracks[0].active, 0);

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);
    CHECK_EQ_I(detections[0].track_id, 2);
}

TEST(t_reset_restarts_track_ids) {
    tpv_vision_tracker tracker;
    tpv_vision_tracker_init(&tracker);
    tpv_vision_config cfg = tracker_config();
    tpv_vision_detection detections[1];

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);
    CHECK_EQ_I(detections[0].track_id, 1);

    tpv_vision_tracker_reset(&tracker);

    detections[0] = make_detection(TPV_ENGINE_ID_TPV_BLOB, 7, 100, 100);
    tpv_vision_tracker_update(&tracker, &cfg, detections, 1);
    CHECK_EQ_I(detections[0].track_id, 1);
}

int main(void) {
    RUN(t_first_hit_tentative_second_hit_confirmed);
    RUN(t_min_hits_one_confirms_immediately);
    RUN(t_slow_motion_keeps_same_track);
    RUN(t_far_jump_creates_new_track);
    RUN(t_global_greedy_matching_is_not_detection_order_dependent);
    RUN(t_engine_and_class_are_isolated);
    RUN(t_empty_frames_age_and_expire_tracks);
    RUN(t_reset_restarts_track_ids);
    FINISH();
}
