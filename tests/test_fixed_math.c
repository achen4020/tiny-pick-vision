#include "tpv_internal.h"
#include "testlib.h"

TEST(t_isqrt_4) {
    /* tpv_isqrt_q16(4.0 in Q16.16) = 2.0 in Q16.16, allow ±10 LSB */
    int64_t r = tpv_isqrt_q16(4LL << 16);
    CHECK(r >= (2LL << 16) - 10 && r <= (2LL << 16) + 10);
}

TEST(t_log_e) {
    /* log(e) ≈ 1.0; e ≈ 2.71828, e * 65536 ≈ 178145 */
    int64_t e_q16 = 178145;
    int32_t lg = tpv_log_q16(e_q16);
    CHECK(lg >= (1 << 16) - 500 && lg <= (1 << 16) + 500);
}

TEST(t_atan2_axis) {
    /* On-axis: atan2(0, +x) = 0, atan2(+y, 0) > 0, atan2(-y, 0) < 0. */
    CHECK(tpv_atan2_q16(0, 1) == 0);
    CHECK(tpv_atan2_q16(1, 0) > 0);
    CHECK(tpv_atan2_q16(-1, 0) < 0);
}

int main(void) {
    RUN(t_isqrt_4);
    RUN(t_log_e);
    RUN(t_atan2_axis);
    FINISH();
}
