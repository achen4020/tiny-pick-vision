#include <string.h>
#include "tpv_internal.h"
#include "tpv.h"
#include "testlib.h"

/* Frame buffers static to avoid 307 KB stack usage in each test. */
static uint8_t y_buf[TPV_WIDTH * TPV_HEIGHT];

TEST(t_empty_frame_returns_empty) {
    memset(y_buf, 0, sizeof y_buf);
    tpv_Detection d;
    int r = tpv_process_frame(y_buf, TPV_WIDTH, TPV_HEIGHT, &d);
    CHECK_EQ_I(r, TPV_EMPTY);
}

TEST(t_bad_input_returns_minus_one) {
    tpv_Detection d;
    int r = tpv_process_frame(NULL, TPV_WIDTH, TPV_HEIGHT, &d);
    CHECK_EQ_I(r, TPV_BAD_INPUT);
    r = tpv_process_frame(y_buf, 100, 100, &d);
    CHECK_EQ_I(r, TPV_BAD_INPUT);
}

TEST(t_blob_exists_but_no_model_is_rejected) {
    /* 100x100 white block centered around (320, 240). With stub model_data
     * (all-zero templates, reject_thresh=0), every blob lands as REJECTED. */
    memset(y_buf, 0, sizeof y_buf);
    for (int yy = 190; yy < 290; yy++)
        for (int xx = 270; xx < 370; xx++) y_buf[yy * TPV_WIDTH + xx] = 255;
    tpv_Detection d;
    int r = tpv_process_frame(y_buf, TPV_WIDTH, TPV_HEIGHT, &d);
    CHECK_EQ_I(r, TPV_OK);
    CHECK_EQ_I(d.class_id, TPV_CLASS_REJECTED);
    CHECK_EQ_I(d.confidence_q8, 0);
    CHECK(d.x > 310 && d.x < 330);
    CHECK(d.y > 230 && d.y < 250);
}

TEST(hg4_reject_still_reports_centroid) {
    /* HG4: TPV_OK + class_id ∈ {0xFE, 0xFF} must still output a usable centroid.
     * 50x50 block at (100..149, 100..149) → centroid ~ (124, 124). */
    memset(y_buf, 0, sizeof y_buf);
    for (int yy = 100; yy < 150; yy++)
        for (int xx = 100; xx < 150; xx++) y_buf[yy * TPV_WIDTH + xx] = 255;
    tpv_Detection d;
    int r = tpv_process_frame(y_buf, TPV_WIDTH, TPV_HEIGHT, &d);
    CHECK_EQ_I(r, TPV_OK);
    CHECK(d.class_id == TPV_CLASS_REJECTED || d.class_id == TPV_CLASS_AMBIGUOUS);
    CHECK(d.x > 110 && d.x < 140);
    CHECK(d.y > 110 && d.y < 140);
    /* theta_x10 may have any value — spec guarantees it's computed but reserved
     * for operator diagnostics, not for grasping. We do not assert on it. */
}

int main(void) {
    RUN(t_empty_frame_returns_empty);
    RUN(t_bad_input_returns_minus_one);
    RUN(t_blob_exists_but_no_model_is_rejected);
    RUN(hg4_reject_still_reports_centroid);
    FINISH();
}
