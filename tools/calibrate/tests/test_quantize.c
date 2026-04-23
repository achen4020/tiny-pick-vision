#include <stdio.h>
#include "tpv_internal.h"
#include "../../../tests/testlib.h"

int tpv_cal_quantize_or_fail(double real, int32_t *q16_out, const char *label);

TEST(hg3_reject_thresh_quantize_to_zero_fails) {
    int32_t q = -1;
    int r = tpv_cal_quantize_or_fail(1e-8, &q, "reject_thresh");
    CHECK_EQ_I(r, -1);
    CHECK_EQ_I(q, -1);   /* must be left untouched */
}

TEST(hg3_margin_quantize_to_zero_fails) {
    int32_t q = -1;
    int r = tpv_cal_quantize_or_fail(1e-9, &q, "margin");
    CHECK_EQ_I(r, -1);
}

TEST(hg3_valid_value_succeeds) {
    int32_t q = 0;
    int r = tpv_cal_quantize_or_fail(2.5, &q, "reject_thresh");
    CHECK_EQ_I(r, 0);
    CHECK_EQ_I(q, (int32_t)(2.5 * 65536));
}

int main(void) {
    RUN(hg3_reject_thresh_quantize_to_zero_fails);
    RUN(hg3_margin_quantize_to_zero_fails);
    RUN(hg3_valid_value_succeeds);
    FINISH();
}
