#include <string.h>
#include "tpv_internal.h"
#include "testlib.h"

/* Build a template with diagonal L⁻¹ = `diag` (Q16.16). Mean is zero unless caller
 * mutates it. reject_thresh and margin are Q16.16. */
static tpv_Template simple_template(int32_t diag, int32_t rt, int32_t mg) {
    tpv_Template t = {0};
    int idx = 0;
    for (int i = 0; i < TPV_N_FEAT; i++) {
        for (int j = 0; j <= i; j++) {
            t.L_inv[idx++] = (i == j) ? diag : 0;
        }
    }
    t.reject_thresh = rt;
    t.margin = mg;
    return t;
}

TEST(t_features_equal_mean_gives_zero_distance) {
    tpv_Template t = simple_template(TPV_Q16, 10 << 16, 2 << 16);
    tpv_Features f = t.mean;
    uint8_t cid = 0xAA, conf = 0xAA; int32_t d1sq = -1;
    tpv_classify(&f, &t, 1, &cid, &conf, &d1sq);
    CHECK_EQ_I(cid, 0);
    CHECK_EQ_I(d1sq, 0);
    CHECK_EQ_I(conf, 255);   /* fit perfect, sep=255 (single class) → conf=255 */
}

TEST(t_far_features_rejected) {
    tpv_Template t = simple_template(TPV_Q16, 4 << 16, 2 << 16);
    tpv_Features f = t.mean;
    f.hu[0] = 5 << 16;       /* d1² = 25 > rt = 4 → REJECTED */
    uint8_t cid, conf; int32_t d1sq;
    tpv_classify(&f, &t, 1, &cid, &conf, &d1sq);
    CHECK_EQ_I(cid, TPV_CLASS_REJECTED);
    CHECK_EQ_I(conf, 0);
}

TEST(t_ambiguous_when_two_classes_close) {
    tpv_Template t[2];
    t[0] = simple_template(TPV_Q16, 10 << 16, 5 << 16);
    t[1] = simple_template(TPV_Q16, 10 << 16, 5 << 16);
    t[1].mean.hu[0] = 2 << 16;   /* place t[1] mean at distance 2 from t[0] */
    tpv_Features f = t[0].mean;
    f.hu[0] = 1 << 15;           /* halfway-ish: d1²(t0)=0.25, d2²(t1)=2.25, gap=2 */
    /* gap 2 < margin 5 → AMBIGUOUS */
    uint8_t cid, conf; int32_t d1sq;
    tpv_classify(&f, t, 2, &cid, &conf, &d1sq);
    CHECK_EQ_I(cid, TPV_CLASS_AMBIGUOUS);
}

TEST(hg1_single_class_never_ambiguous) {
    /* HG1: with N_CLASSES=1 and margin=0, no input may be classified AMBIGUOUS. */
    tpv_Template t = simple_template(TPV_Q16, 10 << 16, 0);
    tpv_Features f = t.mean;
    for (int k = -5; k <= 5; k++) {
        f.hu[0] = k << 16;
        uint8_t cid, conf; int32_t d1sq;
        tpv_classify(&f, &t, 1, &cid, &conf, &d1sq);
        CHECK(cid != TPV_CLASS_AMBIGUOUS);
        if (cid == 0) {
            int32_t fit_q8 = (int32_t)((int64_t)255 * ((10 << 16) - d1sq) / (10 << 16));
            if (fit_q8 < 1) fit_q8 = 1;
            CHECK(conf == (uint8_t)fit_q8 || conf == 255);
        }
    }
}

TEST(hg2_boundary_d1sq_equals_reject_thresh_is_rejected) {
    /* HG2: d1² == reject_thresh must land in REJECTED (closed boundary). */
    const int32_t rt = 16 << 16;
    tpv_Template t = simple_template(TPV_Q16, rt, 4 << 16);
    tpv_Features f = t.mean;
    f.hu[0] = 4 << 16;       /* yields d1² == rt exactly */
    uint8_t cid, conf; int32_t d1sq;
    tpv_classify(&f, &t, 1, &cid, &conf, &d1sq);
    CHECK_EQ_I(d1sq, rt);
    CHECK_EQ_I(cid, TPV_CLASS_REJECTED);
    CHECK_EQ_I(conf, 0);
}

int main(void) {
    RUN(t_features_equal_mean_gives_zero_distance);
    RUN(t_far_features_rejected);
    RUN(t_ambiguous_when_two_classes_close);
    RUN(hg1_single_class_never_ambiguous);
    RUN(hg2_boundary_d1sq_equals_reject_thresh_is_rejected);
    FINISH();
}
