/* Build with -DTPV_DEBUG_FEATURES. Verifies the v2 debug entry, the
 * three-layer mask inclusion invariant, dark-object-mode semantics, ROI
 * clipping, and v1 <-> v2 decision parity. */
#include <string.h>
#include "tpv.h"
#include "tpv_internal.h"
#include "tpv_vision.h"
#include "testlib.h"

static uint8_t frame[TPV_WIDTH * TPV_HEIGHT];

/* Count set bits in an LSB-first packed bitmap. */
static int popcount_bits(const uint8_t *b, int nbytes) {
    int c = 0;
    for (int i = 0; i < nbytes; i++) c += __builtin_popcount((unsigned)b[i]);
    return c;
}

static int mask_xmax(const uint8_t *b) {
    int xmax = -1;
    for (int y = 0; y < TPV_HEIGHT; y++) {
        for (int x = 0; x < TPV_WIDTH; x++) {
            int i = y * TPV_WIDTH + x;
            if ((b[i >> 3] >> (i & 7)) & 1) xmax = x;
        }
    }
    return xmax;
}

static int bit_subset(const uint8_t *sub, const uint8_t *super, int nbytes) {
    for (int i = 0; i < nbytes; i++) {
        if ((sub[i] & ~super[i]) != 0) return 0;
    }
    return 1;
}

static void paint_bright_square(int cx, int cy, int half) {
    memset(frame, 0, sizeof frame);
    for (int y = cy - half; y < cy + half; y++)
        for (int x = cx - half; x < cx + half; x++)
            frame[y * TPV_WIDTH + x] = 255;
}

static void paint_dark_square_on_bright_bg(int cx, int cy, int half) {
    memset(frame, 200, sizeof frame);
    for (int y = cy - half; y < cy + half; y++)
        for (int x = cx - half; x < cx + half; x++)
            frame[y * TPV_WIDTH + x] = 30;
}

static void paint_glossy_dark_rect_on_bright_bg(void) {
    memset(frame, 220, sizeof frame);
    for (int y = 180; y < 300; y++)
        for (int x = 180; x < 460; x++)
            frame[y * TPV_WIDTH + x] = 70;
    /* Simulate a glossy highlight band inside the same physical object:
     * too bright for the strong dark threshold, still darker than paper. */
    for (int y = 220; y < 250; y++)
        for (int x = 180; x < 460; x++)
            frame[y * TPV_WIDTH + x] = 160;
}

static void paint_strong_square_with_many_weak_noise(void) {
    memset(frame, 220, sizeof frame);
    for (int y = 220; y < 260; y++)
        for (int x = 300; x < 340; x++)
            frame[y * TPV_WIDTH + x] = 70;
    for (int y = 0; y < TPV_HEIGHT; y += 20) {
        for (int x = 0; x < TPV_WIDTH; x += 20) {
            if (x >= 280 && x < 360 && y >= 200 && y < 280) continue;
            frame[y * TPV_WIDTH + x] = 180;
        }
    }
}

static void paint_center_phone_with_left_border_distractor(void) {
    memset(frame, 220, sizeof frame);
    for (int y = 180; y < 300; y++)
        for (int x = 180; x < 460; x++)
            frame[y * TPV_WIDTH + x] = 70;
    /* Dark table/background touching the frame edge. Scan order sees this
     * before the centered object, which used to win under all-rejected stub
     * model fallback. */
    for (int y = 120; y < 430; y++)
        for (int x = 0; x < 70; x++)
            frame[y * TPV_WIDTH + x] = 40;
}

static void paint_split_center_object_with_white_highlight_gap(void) {
    memset(frame, 220, sizeof frame);
    for (int y = 160; y < 250; y++)
        for (int x = 170; x < 470; x++)
            frame[y * TPV_WIDTH + x] = 70;
    for (int y = 290; y < 380; y++)
        for (int x = 170; x < 470; x++)
            frame[y * TPV_WIDTH + x] = 70;
}

static void paint_sloped_dark_rect_on_bright_bg(void) {
    memset(frame, 220, sizeof frame);
    for (int y = 180; y < 300; y++) {
        int x0 = 180 + (y - 180) / 4;
        int x1 = x0 + 280;
        for (int x = x0; x < x1; x++)
            frame[y * TPV_WIDTH + x] = 70;
    }
}

static void paint_dark_rect_with_cast_shadow(void) {
    memset(frame, 220, sizeof frame);
    for (int y = 180; y < 300; y++)
        for (int x = 180; x < 420; x++)
            frame[y * TPV_WIDTH + x] = 70;
    for (int y = 190; y < 290; y++)
        for (int x = 420; x < 500; x++)
            frame[y * TPV_WIDTH + x] = 110;
}

TEST(t_v2_bright_square_matches_v1_decision) {
    paint_bright_square(320, 240, 30);

    tpv_Detection prod;
    int rc1 = tpv_process_frame(frame, TPV_WIDTH, TPV_HEIGHT, &prod);

    tpv_DetectionDebugV2 v2;
    int rc2 = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT,
        /* bin_threshold */ 128,
        /* dark_object_mode */ 0,
        /* roi */ 0, 0, TPV_WIDTH, TPV_HEIGHT,
        &v2);

    CHECK_EQ_I(rc1, rc2);
    CHECK_EQ_I(prod.class_id, v2.det.class_id);
    CHECK_EQ_I(prod.x, v2.det.x);
    CHECK_EQ_I(prod.y, v2.det.y);
    CHECK_EQ_I(prod.theta_x10, v2.det.theta_x10);
    CHECK_EQ_I(prod.confidence_q8, v2.det.confidence_q8);
}

TEST(t_v2_dark_object_mode_inverts_threshold) {
    /* White background Y=200, 40x40 dark (Y=30) object at (320, 240) */
    paint_dark_square_on_bright_bg(320, 240, 20);

    /* dark_mode=0: white background > 128 -> foreground, ~307200-1600 px > AMAX -> rejected
     * dark object < 128 -> background -> not seen -> TPV_EMPTY */
    tpv_DetectionDebugV2 v2a;
    int rc_a = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 0,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2a);
    CHECK_EQ_I(rc_a, TPV_EMPTY);

    /* dark_mode=1: white background > 128 -> background, dark object < 128 -> foreground
     * 40x40 = 1600 px in [AMIN=500, AMAX=50000] -> TPV_OK */
    tpv_DetectionDebugV2 v2b;
    int rc_b = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 1,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2b);
    CHECK_EQ_I(rc_b, TPV_OK);
    CHECK(v2b.area_px >= 1500 && v2b.area_px <= 1700);
}

TEST(t_v2_roi_clips_outside_blobs) {
    /* Bright 60x60 square centered at (320, 240). ROI at top-left only. */
    paint_bright_square(320, 240, 30);

    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 0,
        /* roi = (0,0,50,50) - nowhere near (320,240) */
        0, 0, 50, 50, &v2);
    CHECK_EQ_I(rc, TPV_EMPTY);
}

TEST(t_v2_mask_matches_detection_area) {
    paint_bright_square(320, 240, 30);
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 0,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    /* 60x60 = 3600 pixels; CCL should detect ~3600 +- small boundary rounding */
    int n_mask = popcount_bits(v2.mask, sizeof v2.mask);
    CHECK(n_mask >= v2.area_px - 5 && n_mask <= v2.area_px + 5);
    CHECK(v2.area_px >= 3500 && v2.area_px <= 3700);
}

TEST(t_v2_mask_allblobs_bin_inclusion) {
    paint_bright_square(320, 240, 30);
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 0,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    /* Three-layer inclusion: mask subset all_blobs_mask subset bin */
    CHECK(bit_subset(v2.mask, v2.all_blobs_mask, sizeof v2.mask));
    CHECK(bit_subset(v2.all_blobs_mask, v2.bin, sizeof v2.bin));
}

TEST(t_v2_hysteresis_recovers_glossy_highlight_band) {
    paint_glossy_dark_rect_on_bright_bg();
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 1,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    CHECK(v2.area_px >= 33000 && v2.area_px <= 33700);
    CHECK(v2.bbox_x1 - v2.bbox_x0 + 1 >= 275);
    CHECK(v2.bbox_y1 - v2.bbox_y0 + 1 >= 115);
    CHECK_EQ_I(popcount_bits(v2.mask, sizeof v2.mask), v2.area_px);
}

TEST(t_v2_hysteresis_falls_back_when_weak_noise_overflows_ccl) {
    paint_strong_square_with_many_weak_noise();
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 1,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    CHECK(v2.area_px >= 1500 && v2.area_px <= 1700);
}

TEST(t_v2_prefers_center_object_over_border_distractor) {
    paint_center_phone_with_left_border_distractor();
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 1,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    CHECK(v2.bbox_x0 >= 170);
    CHECK(v2.bbox_x1 <= 470);
    CHECK(v2.area_px >= 33000 && v2.area_px <= 33700);
}

TEST(t_v2_groups_split_center_object_components) {
    paint_split_center_object_with_white_highlight_gap();
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 1,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    CHECK(v2.area_px >= 65000 && v2.area_px <= 67000);
    CHECK(v2.bbox_x0 <= 175);
    CHECK(v2.bbox_x1 >= 465);
    CHECK(v2.bbox_y0 <= 165);
    CHECK(v2.bbox_y1 >= 375);
    CHECK_EQ_I(popcount_bits(v2.mask, sizeof v2.mask), v2.area_px);
}

TEST(t_v2_span_fill_preserves_sloped_edges) {
    paint_sloped_dark_rect_on_bright_bg();
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 1,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    int bbox_area = (v2.bbox_x1 - v2.bbox_x0 + 1) *
                    (v2.bbox_y1 - v2.bbox_y0 + 1);
    CHECK(v2.area_px >= 33000 && v2.area_px <= 34500);
    CHECK(bbox_area - v2.area_px >= 2500);
    CHECK_EQ_I(popcount_bits(v2.mask, sizeof v2.mask), v2.area_px);
}

TEST(t_v2_relaxed_threshold_does_not_absorb_cast_shadow) {
    paint_dark_rect_with_cast_shadow();
    tpv_DetectionDebugV2 v2;
    int rc = tpv_process_frame_debug_v2(
        frame, TPV_WIDTH, TPV_HEIGHT, 128, 1,
        0, 0, TPV_WIDTH, TPV_HEIGHT, &v2);
    CHECK_EQ_I(rc, TPV_OK);
    CHECK(v2.area_px >= 28500 && v2.area_px <= 29000);
    CHECK(mask_xmax(v2.mask) < 430);
    CHECK_EQ_I(popcount_bits(v2.mask, sizeof v2.mask), v2.area_px);
}

TEST(t_vision_api_uses_runtime_v2_config) {
    paint_dark_square_on_bright_bg(320, 240, 20);

    tpv_vision_config cfg;
    tpv_vision_default_config(&cfg);
    cfg.bin_threshold = 128;
    cfg.dark_object_mode = 1;
    cfg.roi_x = 250;
    cfg.roi_y = 170;
    cfg.roi_w = 140;
    cfg.roi_h = 140;

    size_t bytes = 0;
    CHECK_EQ_I(tpv_vision_context_size(&cfg, &bytes), TPV_OK);
    uint8_t storage[4096];
    tpv_vision_context *ctx = 0;
    CHECK_EQ_I(tpv_vision_init(storage, sizeof storage, &cfg, &ctx), TPV_OK);

    tpv_vision_frame vf;
    memset(&vf, 0, sizeof vf);
    vf.data = frame;
    vf.width = TPV_WIDTH;
    vf.height = TPV_HEIGHT;
    vf.stride = TPV_WIDTH;
    vf.format = TPV_PIXEL_Y8_640X480;

    tpv_vision_detection detections[1];
    tpv_vision_result result;
    memset(&result, 0, sizeof result);
    result.detections = detections;
    result.detection_capacity = 1;

    CHECK_EQ_I(tpv_vision_process(ctx, &vf, &result), TPV_OK);
    CHECK_EQ_I(result.detection_count, 1);
    CHECK_EQ_I(detections[0].track_id, 1);
    CHECK(detections[0].flags & TPV_DETECTION_HAS_BBOX);
    CHECK(detections[0].bbox_w >= 35 && detections[0].bbox_w <= 45);
    CHECK(detections[0].bbox_h >= 35 && detections[0].bbox_h <= 45);
    CHECK(detections[0].flags & TPV_DETECTION_TRACK_TENTATIVE);
}

int main(void) {
    RUN(t_v2_bright_square_matches_v1_decision);
    RUN(t_v2_dark_object_mode_inverts_threshold);
    RUN(t_v2_roi_clips_outside_blobs);
    RUN(t_v2_mask_matches_detection_area);
    RUN(t_v2_mask_allblobs_bin_inclusion);
    RUN(t_v2_hysteresis_recovers_glossy_highlight_band);
    RUN(t_v2_hysteresis_falls_back_when_weak_noise_overflows_ccl);
    RUN(t_v2_prefers_center_object_over_border_distractor);
    RUN(t_v2_groups_split_center_object_components);
    RUN(t_v2_span_fill_preserves_sloped_edges);
    RUN(t_v2_relaxed_threshold_does_not_absorb_cast_shadow);
    RUN(t_vision_api_uses_runtime_v2_config);
    FINISH();
}
