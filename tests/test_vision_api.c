#include <string.h>
#include "tpv_config.h"
#include "tpv_vision.h"
#include "testlib.h"

_Static_assert(sizeof(tpv_vision_config) == TPV_VISION_CONFIG_SIZE,
               "tpv_vision_config size drift");
_Static_assert(sizeof(tpv_vision_detection) == TPV_VISION_DETECTION_SIZE,
               "tpv_vision_detection size drift");

static uint8_t frame[TPV_WIDTH * TPV_HEIGHT];

static void paint_bright_square(int cx, int cy, int half) {
    memset(frame, 0, sizeof frame);
    for (int y = cy - half; y < cy + half; y++) {
        for (int x = cx - half; x < cx + half; x++) {
            frame[y * TPV_WIDTH + x] = 255;
        }
    }
}

static tpv_vision_context *make_context(uint8_t *storage, size_t storage_size) {
    tpv_vision_config cfg;
    tpv_vision_default_config(&cfg);
    tpv_vision_context *ctx = 0;
    int rc = tpv_vision_init(storage, storage_size, &cfg, &ctx);
    return (rc == TPV_OK) ? ctx : 0;
}

static tpv_vision_frame y8_frame(void) {
    tpv_vision_frame f;
    memset(&f, 0, sizeof f);
    f.data = frame;
    f.width = TPV_WIDTH;
    f.height = TPV_HEIGHT;
    f.stride = TPV_WIDTH;
    f.format = TPV_PIXEL_Y8_640X480;
    f.rotation_degrees = 0;
    f.timestamp_ns = 123;
    return f;
}

TEST(t_default_config_initializes_tpv_only_mode) {
    tpv_vision_config cfg;
    memset(&cfg, 0xAA, sizeof cfg);

    tpv_vision_default_config(&cfg);

    CHECK_EQ_I(cfg.abi_version, TPV_VISION_ABI_VERSION);
    CHECK_EQ_I(cfg.enabled_engines, TPV_ENGINE_FLAG_TPV_BLOB);
    CHECK_EQ_I(cfg.primary_event_engine, TPV_ENGINE_ID_TPV_BLOB);
    CHECK_EQ_I(cfg.roi_x, 0);
    CHECK_EQ_I(cfg.roi_y, 0);
    CHECK_EQ_I(cfg.roi_w, TPV_WIDTH);
    CHECK_EQ_I(cfg.roi_h, TPV_HEIGHT);
}

TEST(t_context_size_and_init_are_caller_owned) {
    tpv_vision_config cfg;
    tpv_vision_default_config(&cfg);
    size_t bytes = 0;

    int rc = tpv_vision_context_size(&cfg, &bytes);
    CHECK_EQ_I(rc, TPV_OK);
    CHECK(bytes > 0);

    uint8_t storage[2048];
    tpv_vision_context *ctx = 0;
    rc = tpv_vision_init(storage, bytes - 1, &cfg, &ctx);
    CHECK_EQ_I(rc, TPV_BAD_INPUT);
    CHECK(ctx == 0);

    rc = tpv_vision_init(storage, sizeof storage, &cfg, &ctx);
    CHECK_EQ_I(rc, TPV_OK);
    CHECK(ctx == (tpv_vision_context *)storage);
}

TEST(t_unsupported_face_engine_is_rejected_in_c0) {
    tpv_vision_config cfg;
    tpv_vision_default_config(&cfg);
    cfg.enabled_engines |= TPV_ENGINE_FLAG_FACE;

    size_t bytes = 0;
    int rc = tpv_vision_context_size(&cfg, &bytes);
    CHECK_EQ_I(rc, TPV_BAD_INPUT);
    CHECK_EQ_I(bytes, 0);
}

TEST(t_empty_frame_returns_empty_with_no_detections) {
    uint8_t storage[2048];
    tpv_vision_context *ctx = make_context(storage, sizeof storage);
    CHECK(ctx != 0);
    memset(frame, 0, sizeof frame);
    tpv_vision_frame f = y8_frame();
    tpv_vision_detection detections[1];
    tpv_vision_result out;
    memset(&out, 0, sizeof out);
    out.detections = detections;
    out.detection_capacity = 1;

    int rc = tpv_vision_process(ctx, &f, &out);

    CHECK_EQ_I(rc, TPV_EMPTY);
    CHECK_EQ_I(out.status, TPV_EMPTY);
    CHECK_EQ_I(out.primary_event_engine, TPV_ENGINE_ID_TPV_BLOB);
    CHECK_EQ_I(out.detection_count, 0);
}

TEST(t_present_frame_emits_tpv_blob_detection) {
    uint8_t storage[2048];
    tpv_vision_context *ctx = make_context(storage, sizeof storage);
    CHECK(ctx != 0);
    paint_bright_square(320, 240, 30);
    tpv_vision_frame f = y8_frame();
    tpv_vision_detection detections[1];
    tpv_vision_result out;
    memset(&out, 0, sizeof out);
    out.detections = detections;
    out.detection_capacity = 1;

    int rc = tpv_vision_process(ctx, &f, &out);

    CHECK_EQ_I(rc, TPV_OK);
    CHECK_EQ_I(out.status, TPV_OK);
    CHECK_EQ_I(out.detection_count, 1);
    CHECK_EQ_I(detections[0].engine_id, TPV_ENGINE_ID_TPV_BLOB);
    CHECK_EQ_I(detections[0].detection_id, 1);
    CHECK_EQ_I(detections[0].track_id, 1);
    CHECK(detections[0].flags & TPV_DETECTION_HAS_CENTER);
    CHECK(detections[0].flags & TPV_DETECTION_HAS_THETA);
    CHECK(detections[0].flags & TPV_DETECTION_PRIMARY);
    CHECK(detections[0].flags & TPV_DETECTION_TRACK_TENTATIVE);
    CHECK_EQ_I(detections[0].class_id, TPV_CLASS_REJECTED);
    CHECK(detections[0].center_x > 300 && detections[0].center_x < 340);
    CHECK(detections[0].center_y > 220 && detections[0].center_y < 260);
}

TEST(t_present_frame_requires_detection_capacity) {
    uint8_t storage[2048];
    tpv_vision_context *ctx = make_context(storage, sizeof storage);
    CHECK(ctx != 0);
    paint_bright_square(320, 240, 30);
    tpv_vision_frame f = y8_frame();
    tpv_vision_result out;
    memset(&out, 0, sizeof out);
    out.detections = 0;
    out.detection_capacity = 0;

    int rc = tpv_vision_process(ctx, &f, &out);

    CHECK_EQ_I(rc, TPV_BAD_INPUT);
    CHECK_EQ_I(out.status, TPV_BAD_INPUT);
    CHECK_EQ_I(out.detection_count, 0);
}

TEST(t_bad_frame_format_returns_bad_input) {
    uint8_t storage[2048];
    tpv_vision_context *ctx = make_context(storage, sizeof storage);
    CHECK(ctx != 0);
    tpv_vision_frame f = y8_frame();
    f.format = TPV_PIXEL_NV21;
    tpv_vision_detection detections[1];
    tpv_vision_result out;
    memset(&out, 0, sizeof out);
    out.detections = detections;
    out.detection_capacity = 1;

    int rc = tpv_vision_process(ctx, &f, &out);

    CHECK_EQ_I(rc, TPV_BAD_INPUT);
    CHECK_EQ_I(out.status, TPV_BAD_INPUT);
    CHECK_EQ_I(out.detection_count, 0);
}

TEST(t_reset_restarts_detection_ids) {
    uint8_t storage[2048];
    tpv_vision_context *ctx = make_context(storage, sizeof storage);
    CHECK(ctx != 0);
    paint_bright_square(320, 240, 30);
    tpv_vision_frame f = y8_frame();
    tpv_vision_detection detections[1];
    tpv_vision_result out;
    memset(&out, 0, sizeof out);
    out.detections = detections;
    out.detection_capacity = 1;

    CHECK_EQ_I(tpv_vision_process(ctx, &f, &out), TPV_OK);
    CHECK_EQ_I(detections[0].detection_id, 1);
    CHECK_EQ_I(detections[0].track_id, 1);
    CHECK_EQ_I(tpv_vision_process(ctx, &f, &out), TPV_OK);
    CHECK_EQ_I(detections[0].detection_id, 2);
    CHECK_EQ_I(detections[0].track_id, 1);

    tpv_vision_reset(ctx);

    CHECK_EQ_I(tpv_vision_process(ctx, &f, &out), TPV_OK);
    CHECK_EQ_I(detections[0].detection_id, 1);
    CHECK_EQ_I(detections[0].track_id, 1);
}

int main(void) {
    RUN(t_default_config_initializes_tpv_only_mode);
    RUN(t_context_size_and_init_are_caller_owned);
    RUN(t_unsupported_face_engine_is_rejected_in_c0);
    RUN(t_empty_frame_returns_empty_with_no_detections);
    RUN(t_present_frame_emits_tpv_blob_detection);
    RUN(t_present_frame_requires_detection_capacity);
    RUN(t_bad_frame_format_returns_bad_input);
    RUN(t_reset_restarts_detection_ids);
    FINISH();
}
