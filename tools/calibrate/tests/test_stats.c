#include <string.h>
#include "tpv_internal.h"
#include "../../../tests/testlib.h"

void tpv_cal_mean_cov(const tpv_Features *samples, int n,
                      int32_t *mean_out, double *cov_out);
int  tpv_cal_cholesky_inv(const double *cov, int32_t *L_inv_q16_out);

TEST(t_mean_cov_trivial) {
    /* 3 samples with hu[0] = 1, 2, 3 (Q16.16). Mean must be 2 in Q16.16,
     * variance ((1-2)² + 0 + (3-2)²) / (n-1) = 1.0. */
    tpv_Features s[3];
    memset(s, 0, sizeof s);
    s[0].hu[0] = 1 << 16;
    s[1].hu[0] = 2 << 16;
    s[2].hu[0] = 3 << 16;
    int32_t mean[TPV_N_FEAT];
    double cov[TPV_N_FEAT * TPV_N_FEAT];
    tpv_cal_mean_cov(s, 3, mean, cov);
    CHECK_EQ_I(mean[0], 2 << 16);
    CHECK(cov[0] > 0.9 && cov[0] < 1.1);
}

TEST(t_cholesky_inv_identity) {
    /* Cov = identity → L = identity → L⁻¹ = identity in Q16.16. */
    double cov[TPV_N_FEAT * TPV_N_FEAT] = {0};
    for (int i = 0; i < TPV_N_FEAT; i++) cov[i*TPV_N_FEAT+i] = 1.0;
    int32_t L_inv[TPV_L_INV_N];
    int r = tpv_cal_cholesky_inv(cov, L_inv);
    CHECK_EQ_I(r, 0);
    /* Diagonal entries should be 1.0 in Q16.16; off-diagonals 0. */
    int idx = 0;
    for (int i = 0; i < TPV_N_FEAT; i++) {
        for (int j = 0; j <= i; j++) {
            int32_t v = L_inv[idx++];
            if (i == j) CHECK(v >= (1 << 16) - 5 && v <= (1 << 16) + 5);
            else        CHECK_EQ_I(v, 0);
        }
    }
}

TEST(t_cholesky_inv_nonpsd_returns_minus_one) {
    /* Negative diagonal → not positive definite. */
    double cov[TPV_N_FEAT * TPV_N_FEAT] = {0};
    for (int i = 0; i < TPV_N_FEAT; i++) cov[i*TPV_N_FEAT+i] = 1.0;
    cov[0] = -1.0;
    int32_t L_inv[TPV_L_INV_N];
    int r = tpv_cal_cholesky_inv(cov, L_inv);
    CHECK_EQ_I(r, -1);
}

int main(void) {
    RUN(t_mean_cov_trivial);
    RUN(t_cholesky_inv_identity);
    RUN(t_cholesky_inv_nonpsd_returns_minus_one);
    FINISH();
}
