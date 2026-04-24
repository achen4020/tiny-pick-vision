/* Build with -DTPV_DEBUG_FEATURES. Verifies the v2 debug entry, the
 * three-layer mask inclusion invariant, dark-object-mode semantics, ROI
 * clipping, and v1 <-> v2 decision parity. */
#include <string.h>
#include "tpv.h"
#include "tpv_internal.h"
#include "testlib.h"

static uint8_t frame[TPV_WIDTH * TPV_HEIGHT];

/* Count set bits in an LSB-first packed bitmap. */
static int popcount_bits(const uint8_t *b, int nbytes) {
    int c = 0;
    for (int i = 0; i < nbytes; i++) c += __builtin_popcount((unsigned)b[i]);
    return c;
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

int main(void) {
    RUN(t_v2_bright_square_matches_v1_decision);
    RUN(t_v2_dark_object_mode_inverts_threshold);
    RUN(t_v2_roi_clips_outside_blobs);
    RUN(t_v2_mask_matches_detection_area);
    RUN(t_v2_mask_allblobs_bin_inclusion);
    FINISH();
}
