#ifndef TPV_VISION_H
#define TPV_VISION_H

#include <stddef.h>
#include <stdint.h>
#include "tpv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TPV_VISION_ABI_VERSION 1u
#define TPV_VISION_CONFIG_SIZE 64u
#define TPV_VISION_DETECTION_SIZE 40u

#define TPV_ENGINE_ID_TPV_BLOB 1u
#define TPV_ENGINE_ID_FACE     2u
#define TPV_ENGINE_ID_OBJECT   3u

#define TPV_ENGINE_FLAG_TPV_BLOB (1u << 0)
#define TPV_ENGINE_FLAG_FACE     (1u << 1)
#define TPV_ENGINE_FLAG_OBJECT   (1u << 2)

#define TPV_PIXEL_Y8_640X480 1u
#define TPV_PIXEL_NV21       2u
#define TPV_PIXEL_RGBA8888   3u

#define TPV_DETECTION_HAS_CENTER  (1u << 0)
#define TPV_DETECTION_HAS_BBOX    (1u << 1)
#define TPV_DETECTION_HAS_THETA   (1u << 2)
#define TPV_DETECTION_PRIMARY     (1u << 3)
#define TPV_DETECTION_TRACK_TENTATIVE (1u << 4)
#define TPV_DETECTION_TRACK_CONFIRMED (1u << 5)
#define TPV_DETECTION_TRACK_LOST      (1u << 6)

typedef struct tpv_vision_context tpv_vision_context;

typedef struct {
    uint32_t abi_version;
    uint32_t enabled_engines;
    uint32_t primary_event_engine;
    uint8_t  bin_threshold;
    uint8_t  dark_object_mode;
    uint8_t  reserved0[2];
    int16_t  roi_x, roi_y, roi_w, roi_h;
    int32_t  tracker_min_hits;
    int32_t  tracker_max_age;
    float    tracker_iou_threshold;
    float    tracker_center_distance_px;
    float    face_min_score;
    float    object_min_score;
    uint32_t reserved1[4];
} tpv_vision_config;

typedef struct {
    const uint8_t *data;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t format;
    int32_t rotation_degrees;
    int64_t timestamp_ns;
} tpv_vision_frame;

typedef struct {
    uint32_t engine_id;
    uint32_t detection_id;
    uint32_t track_id;
    uint16_t flags;
    uint16_t class_id;
    uint8_t  confidence_q8;
    int8_t   status;
    int16_t  center_x, center_y;
    int16_t  bbox_x, bbox_y, bbox_w, bbox_h;
    int16_t  theta_x10;
    int16_t  track_age_frames;
    int16_t  track_hits;
    int16_t  track_misses;
    int16_t  reserved0;
} tpv_vision_detection;

typedef struct {
    int32_t status;
    uint32_t primary_event_engine;
    tpv_vision_detection *detections;
    int32_t detection_capacity;
    int32_t detection_count;
    uint32_t reserved0[4];
} tpv_vision_result;

void tpv_vision_default_config(tpv_vision_config *out);
int tpv_vision_context_size(const tpv_vision_config *cfg, size_t *bytes_out);
int tpv_vision_init(void *mem, size_t bytes,
                    const tpv_vision_config *cfg,
                    tpv_vision_context **ctx_out);
void tpv_vision_reset(tpv_vision_context *ctx);
int tpv_vision_process(tpv_vision_context *ctx,
                       const tpv_vision_frame *frame,
                       tpv_vision_result *out);

#ifdef __cplusplus
}
#endif

#endif
