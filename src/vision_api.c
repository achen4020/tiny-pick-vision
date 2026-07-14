#include <string.h>
#include "tpv_config.h"
#include "tpv_internal.h"
#include "tpv_vision.h"
#include "vision_policy.h"
#include "vision_tracker.h"

struct tpv_vision_context {
    tpv_vision_config config;
    uint32_t next_detection_id;
    tpv_vision_tracker tracker;
};

#ifdef TPV_DEBUG_FEATURES
static tpv_DetectionDebugV2 s_debug_v2;
static int s_debug_v2_status = TPV_BAD_INPUT;
static uint8_t s_debug_v2_valid;
static const tpv_vision_context *s_debug_v2_owner;
#endif

static uint32_t engine_id_to_flag(uint32_t engine_id) {
    switch (engine_id) {
    case TPV_ENGINE_ID_TPV_BLOB: return TPV_ENGINE_FLAG_TPV_BLOB;
    case TPV_ENGINE_ID_FACE:     return TPV_ENGINE_FLAG_FACE;
    case TPV_ENGINE_ID_OBJECT:   return TPV_ENGINE_FLAG_OBJECT;
    default: return 0;
    }
}

static int validate_config(const tpv_vision_config *cfg) {
    if (!cfg) return TPV_BAD_INPUT;
    if (cfg->abi_version != TPV_VISION_ABI_VERSION) return TPV_BAD_INPUT;
    if ((cfg->enabled_engines & TPV_ENGINE_FLAG_TPV_BLOB) == 0) return TPV_BAD_INPUT;
    if ((cfg->enabled_engines & ~(uint32_t)TPV_ENGINE_FLAG_TPV_BLOB) != 0) {
        return TPV_BAD_INPUT;
    }
    uint32_t primary_flag = engine_id_to_flag(cfg->primary_event_engine);
    if (primary_flag == 0 || (cfg->enabled_engines & primary_flag) == 0) {
        return TPV_BAD_INPUT;
    }
    if (cfg->roi_x < 0 || cfg->roi_y < 0 || cfg->roi_w <= 0 || cfg->roi_h <= 0) {
        return TPV_BAD_INPUT;
    }
    if (cfg->roi_x + cfg->roi_w > TPV_WIDTH || cfg->roi_y + cfg->roi_h > TPV_HEIGHT) {
        return TPV_BAD_INPUT;
    }
    if (cfg->tracker_min_hits < 1 || cfg->tracker_max_age < 0) return TPV_BAD_INPUT;
    if (cfg->tracker_iou_threshold < 0.0f || cfg->tracker_iou_threshold > 1.0f) {
        return TPV_BAD_INPUT;
    }
    if (cfg->tracker_center_distance_px < 0.0f) return TPV_BAD_INPUT;
    if (cfg->face_min_score < 0.0f || cfg->face_min_score > 1.0f) return TPV_BAD_INPUT;
    if (cfg->object_min_score < 0.0f || cfg->object_min_score > 1.0f) return TPV_BAD_INPUT;
    return TPV_OK;
}

static int validate_y8_frame(const tpv_vision_frame *frame) {
    if (!frame || !frame->data) return TPV_BAD_INPUT;
    if (frame->format != TPV_PIXEL_Y8_640X480) return TPV_BAD_INPUT;
    if (frame->width != TPV_WIDTH || frame->height != TPV_HEIGHT) return TPV_BAD_INPUT;
    if (frame->stride != TPV_WIDTH) return TPV_BAD_INPUT;
    return TPV_OK;
}

#ifndef TPV_DEBUG_FEATURES
static int compact_tpv_config_is_supported(const tpv_vision_config *cfg) {
    return cfg->bin_threshold == TPV_BIN_THRESH_DEFAULT &&
           cfg->dark_object_mode == 0 &&
           cfg->roi_x == 0 &&
           cfg->roi_y == 0 &&
           cfg->roi_w == TPV_WIDTH &&
           cfg->roi_h == TPV_HEIGHT;
}
#endif

static void fill_tpv_detection(tpv_vision_context *ctx,
                               tpv_vision_detection *vd,
                               const tpv_Detection *det,
                               int status) {
    memset(vd, 0, sizeof *vd);
    vd->engine_id = TPV_ENGINE_ID_TPV_BLOB;
    vd->detection_id = ctx->next_detection_id++;
    vd->flags = TPV_DETECTION_HAS_CENTER |
                TPV_DETECTION_HAS_THETA |
                TPV_DETECTION_PRIMARY;
    vd->class_id = det->class_id;
    vd->confidence_q8 = det->confidence_q8;
    vd->status = (int8_t)status;
    vd->center_x = det->x;
    vd->center_y = det->y;
    vd->theta_x10 = det->theta_x10;
}

static void result_reset(tpv_vision_result *out, int status) {
    if (!out) return;
    out->status = status;
    out->primary_event_engine = 0;
    out->detection_count = 0;
    memset(out->reserved0, 0, sizeof out->reserved0);
}

void tpv_vision_default_config(tpv_vision_config *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->abi_version = TPV_VISION_ABI_VERSION;
    out->enabled_engines = TPV_ENGINE_FLAG_TPV_BLOB;
    out->primary_event_engine = TPV_ENGINE_ID_TPV_BLOB;
    out->bin_threshold = TPV_BIN_THRESH_DEFAULT;
    out->dark_object_mode = 0;
    out->roi_x = 0;
    out->roi_y = 0;
    out->roi_w = TPV_WIDTH;
    out->roi_h = TPV_HEIGHT;
    out->tracker_min_hits = 2;
    out->tracker_max_age = 10;
    out->tracker_iou_threshold = 0.25f;
    out->tracker_center_distance_px = 80.0f;
    out->face_min_score = 0.50f;
    out->object_min_score = 0.50f;
}

int tpv_vision_context_size(const tpv_vision_config *cfg, size_t *bytes_out) {
    if (!bytes_out) return TPV_BAD_INPUT;
    *bytes_out = 0;
    int rc = validate_config(cfg);
    if (rc != TPV_OK) return rc;
    *bytes_out = sizeof(tpv_vision_context);
    return TPV_OK;
}

int tpv_vision_init(void *mem, size_t bytes,
                    const tpv_vision_config *cfg,
                    tpv_vision_context **ctx_out) {
    if (!mem || !ctx_out) return TPV_BAD_INPUT;
    *ctx_out = 0;
    int rc = validate_config(cfg);
    if (rc != TPV_OK) return rc;
    if (bytes < sizeof(tpv_vision_context)) return TPV_BAD_INPUT;

    tpv_vision_context *ctx = (tpv_vision_context *)mem;
#ifdef TPV_DEBUG_FEATURES
    if (s_debug_v2_owner == ctx) {
        memset(&s_debug_v2, 0, sizeof s_debug_v2);
        s_debug_v2_status = TPV_BAD_INPUT;
        s_debug_v2_valid = 0;
        s_debug_v2_owner = 0;
    }
#endif
    memset(ctx, 0, sizeof *ctx);
    ctx->config = *cfg;
    ctx->next_detection_id = 1;
    tpv_vision_tracker_init(&ctx->tracker);
    *ctx_out = ctx;
    return TPV_OK;
}

void tpv_vision_reset(tpv_vision_context *ctx) {
    if (!ctx) return;
    ctx->next_detection_id = 1;
    tpv_vision_tracker_reset(&ctx->tracker);
#ifdef TPV_DEBUG_FEATURES
    if (s_debug_v2_owner == ctx) {
        memset(&s_debug_v2, 0, sizeof s_debug_v2);
        s_debug_v2_status = TPV_BAD_INPUT;
        s_debug_v2_valid = 0;
        s_debug_v2_owner = 0;
    }
#endif
}

int tpv_vision_process(tpv_vision_context *ctx,
                       const tpv_vision_frame *frame,
                       tpv_vision_result *out) {
    if (!ctx || !out) return TPV_BAD_INPUT;
    result_reset(out, TPV_BAD_INPUT);
    out->primary_event_engine = ctx->config.primary_event_engine;

#ifdef TPV_DEBUG_FEATURES
    if (s_debug_v2_owner == ctx) s_debug_v2_valid = 0;
#endif

    int rc = validate_y8_frame(frame);
    if (rc != TPV_OK) return rc;

#ifdef TPV_DEBUG_FEATURES
    rc = tpv_process_frame_debug_v2(
        frame->data, frame->width, frame->height,
        ctx->config.bin_threshold,
        ctx->config.dark_object_mode ? 1 : 0,
        ctx->config.roi_x, ctx->config.roi_y,
        ctx->config.roi_w, ctx->config.roi_h,
        &s_debug_v2);
    s_debug_v2_status = rc;
    s_debug_v2_valid = 1;
    s_debug_v2_owner = ctx;
#else
    if (!compact_tpv_config_is_supported(&ctx->config)) {
        out->status = TPV_BAD_INPUT;
        return TPV_BAD_INPUT;
    }
    tpv_Detection det;
    rc = tpv_process_frame(frame->data, frame->width, frame->height, &det);
#endif
    out->status = rc;
    if (rc != TPV_OK) {
        if (rc == TPV_EMPTY || rc == TPV_SCENE_ERROR) {
            tpv_vision_tracker_update(&ctx->tracker, &ctx->config, 0, 0);
        }
        return rc;
    }

    if (!out->detections || out->detection_capacity < 1) {
        out->status = TPV_BAD_INPUT;
        return TPV_BAD_INPUT;
    }

    tpv_vision_detection *vd = &out->detections[0];
#ifdef TPV_DEBUG_FEATURES
    fill_tpv_detection(ctx, vd, &s_debug_v2.det, rc);
    vd->flags |= TPV_DETECTION_HAS_BBOX;
    vd->bbox_x = s_debug_v2.bbox_x0;
    vd->bbox_y = s_debug_v2.bbox_y0;
    vd->bbox_w = s_debug_v2.bbox_x1 - s_debug_v2.bbox_x0 + 1;
    vd->bbox_h = s_debug_v2.bbox_y1 - s_debug_v2.bbox_y0 + 1;
#else
    fill_tpv_detection(ctx, vd, &det, rc);
#endif
    out->detection_count = 1;
    tpv_vision_tracker_update(&ctx->tracker, &ctx->config,
                              out->detections, out->detection_count);
    tpv_vision_policy_apply(&ctx->config, out->detections, out->detection_count);
    return TPV_OK;
}

#ifdef TPV_DEBUG_FEATURES
int tpv_vision_last_debug_v2(const tpv_vision_context *ctx,
                             tpv_DetectionDebugV2 *out) {
    if (!ctx || !out || !s_debug_v2_valid || s_debug_v2_owner != ctx) {
        return TPV_BAD_INPUT;
    }
    *out = s_debug_v2;
    return s_debug_v2_status;
}
#endif
