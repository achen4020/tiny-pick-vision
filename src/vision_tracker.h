#ifndef TPV_VISION_TRACKER_H
#define TPV_VISION_TRACKER_H

#include <stdint.h>
#include "tpv_vision.h"

#define TPV_VISION_MAX_TRACKS 16

typedef struct {
    uint8_t active;
    uint8_t confirmed;
    uint8_t matched;
    uint8_t reserved0;
    uint32_t id;
    uint32_t engine_id;
    uint16_t class_id;
    uint16_t reserved1;
    int32_t age_frames;
    int32_t hits;
    int32_t misses;
    tpv_vision_detection detection;
} tpv_vision_track;

typedef struct {
    uint32_t next_track_id;
    tpv_vision_track tracks[TPV_VISION_MAX_TRACKS];
} tpv_vision_tracker;

void tpv_vision_tracker_init(tpv_vision_tracker *tracker);
void tpv_vision_tracker_reset(tpv_vision_tracker *tracker);
void tpv_vision_tracker_update(tpv_vision_tracker *tracker,
                               const tpv_vision_config *cfg,
                               tpv_vision_detection *detections,
                               int detection_count);

#endif
