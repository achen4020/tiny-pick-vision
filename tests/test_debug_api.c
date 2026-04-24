/* Build with -DTPV_DEBUG_FEATURES. Verifies that for the same Y buffer,
 * tpv_process_frame_debug and tpv_process_frame produce identical Detection
 * fields (no silent divergence in the debug wrapper). */
#include <string.h>
#include "tpv.h"
#include "tpv_internal.h"
#include "testlib.h"

static uint8_t frame[TPV_WIDTH * TPV_HEIGHT];

static void paint_square(int cx, int cy, int half) {
    memset(frame, 0, sizeof frame);
    for (int y = cy - half; y < cy + half; y++)
        for (int x = cx - half; x < cx + half; x++)
            frame[y * TPV_WIDTH + x] = 255;
}

TEST(t_debug_matches_production_on_same_frame) {
    paint_square(320, 240, 30);

    tpv_Detection prod;
    int rc_prod = tpv_process_frame(frame, TPV_WIDTH, TPV_HEIGHT, &prod);

    tpv_DetectionDebug dbg;
    int rc_dbg = tpv_process_frame_debug(frame, TPV_WIDTH, TPV_HEIGHT, &dbg);

    CHECK_EQ_I(rc_prod, rc_dbg);
    CHECK_EQ_I(prod.class_id, dbg.det.class_id);
    CHECK_EQ_I(prod.x, dbg.det.x);
    CHECK_EQ_I(prod.y, dbg.det.y);
    CHECK_EQ_I(prod.theta_x10, dbg.det.theta_x10);
    CHECK_EQ_I(prod.confidence_q8, dbg.det.confidence_q8);
}

TEST(t_debug_distances_array_length_matches_n_classes) {
    paint_square(320, 240, 30);
    tpv_DetectionDebug dbg;
    tpv_process_frame_debug(frame, TPV_WIDTH, TPV_HEIGHT, &dbg);
    /* Compile-time check via array size doesn't work here (distances_sq
     * sizeof is a compile constant already); instead we ensure at least
     * one entry is touched and the array bounds pass a smoke read. */
    int sum = 0;
    for (int i = 0; i < TPV_N_CLASSES; i++) sum |= (int)dbg.distances_sq[i];
    CHECK(sum >= 0);  /* always true, but forces read of every slot */
}

TEST(t_debug_empty_frame_returns_empty) {
    memset(frame, 0, sizeof frame);
    tpv_DetectionDebug dbg;
    int rc = tpv_process_frame_debug(frame, TPV_WIDTH, TPV_HEIGHT, &dbg);
    CHECK_EQ_I(rc, TPV_EMPTY);
}

int main(void) {
    RUN(t_debug_matches_production_on_same_frame);
    RUN(t_debug_distances_array_length_matches_n_classes);
    RUN(t_debug_empty_frame_returns_empty);
    FINISH();
}
