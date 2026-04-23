/* Property tests: invariance properties of the pipeline that should hold
 * regardless of input pose. */
#include "tpv.h"
#include "tpv_internal.h"
#include "testlib.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint8_t y_buf[TPV_WIDTH * TPV_HEIGHT];
static uint8_t bin_buf[TPV_WIDTH * TPV_HEIGHT / 8];
static tpv_Blob blobs_buf[4];

/* L shape rotated by theta_deg: 50×50 block + 30×10 foot pointing along +x. */
static void render_L(uint8_t *y, double theta_deg) {
    memset(y, 0, TPV_WIDTH * TPV_HEIGHT);
    double c = cos(theta_deg * M_PI / 180.0);
    double s = sin(theta_deg * M_PI / 180.0);
    for (int dy = -25; dy < 25; dy++)
        for (int dx = -25; dx < 25; dx++) {
            int px = (int)(320 + c*dx - s*dy);
            int py = (int)(240 + s*dx + c*dy);
            if (px >= 0 && px < TPV_WIDTH && py >= 0 && py < TPV_HEIGHT)
                y[py * TPV_WIDTH + px] = 255;
        }
    for (int dy = -5; dy < 5; dy++)
        for (int dx = 25; dx < 55; dx++) {
            int px = (int)(320 + c*dx - s*dy);
            int py = (int)(240 + s*dx + c*dy);
            if (px >= 0 && px < TPV_WIDTH && py >= 0 && py < TPV_HEIGHT)
                y[py * TPV_WIDTH + px] = 255;
        }
}

/* Rotation of input ⇒ θ output rotates with it.
 *
 * The pipeline's principal-axis math gives θ modulo π (range [-90°, 90°]); the
 * 180° disambig uses a simplified μ₃ projection. Match within ±5° AFTER
 * collapsing both to mod-180. The full-360° version would require the cubic
 * μ₃-along-principal-axis projection (spec §7) which is deferred. */
TEST(t_rotation_axis_tracks_input_mod_180) {
    for (double th = -80; th <= 80; th += 20) {
        render_L(y_buf, th);
        tpv_Detection d;
        int r = tpv_process_frame(y_buf, TPV_WIDTH, TPV_HEIGHT, &d);
        CHECK_EQ_I(r, TPV_OK);
        /* Collapse both to [-900, 900) (i.e. mod 180°×10 = mod 1800). */
        int got = d.theta_x10 % 1800;
        if (got >= 900) got -= 1800;
        if (got < -900) got += 1800;
        int expected = (int)(th * 10);
        int diff = got - expected;
        if (diff >= 900) diff -= 1800;
        if (diff < -900) diff += 1800;
        CHECK(diff >= -50 && diff <= 50);     /* allow ±5° slack for renderer */
    }
}

static void render_square_at(uint8_t *y, int cx, int cy) {
    memset(y, 0, TPV_WIDTH * TPV_HEIGHT);
    for (int dy = -20; dy < 20; dy++)
        for (int dx = -20; dx < 20; dx++) {
            int px = cx + dx, py = cy + dy;
            if (px >= 0 && px < TPV_WIDTH && py >= 0 && py < TPV_HEIGHT)
                y[py * TPV_WIDTH + px] = 255;
        }
}

/* Translation of input ⇒ Hu features identical (rotation/scale/translation
 * invariants — translation in particular should match almost exactly). */
TEST(t_translation_invariance_features) {
    tpv_Features f1, f2;

    render_square_at(y_buf, 100, 100);
    tpv_threshold(y_buf, TPV_WIDTH, TPV_HEIGHT, bin_buf);
    int n = tpv_ccl_moments(bin_buf, TPV_WIDTH, TPV_HEIGHT, blobs_buf, 4);
    CHECK_EQ_I(n, 1);
    tpv_shape_features(&blobs_buf[0], &f1);

    render_square_at(y_buf, 500, 400);
    tpv_threshold(y_buf, TPV_WIDTH, TPV_HEIGHT, bin_buf);
    n = tpv_ccl_moments(bin_buf, TPV_WIDTH, TPV_HEIGHT, blobs_buf, 4);
    CHECK_EQ_I(n, 1);
    tpv_shape_features(&blobs_buf[0], &f2);

    for (int i = 0; i < 7; i++) {
        int32_t diff = f1.hu[i] - f2.hu[i];
        if (diff < 0) diff = -diff;
        CHECK(diff < 1024);     /* ±1.5e-2 in Q16.16 */
    }
}

int main(void) {
    RUN(t_rotation_axis_tracks_input_mod_180);
    RUN(t_translation_invariance_features);
    FINISH();
}
