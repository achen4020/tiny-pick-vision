#include <stdio.h>
#include "tpv_internal.h"

double tpv_cal_mahal_sq(const tpv_Features *x, const int32_t *mean_q16,
                        const int32_t *L_inv_q16);

/* Spec §8 step 8: for each pair (i,j) check that the squared Mahalanobis
 * distance from μ_j (under c_i's metric, and vice versa) is comfortably
 * larger than max(reject_thresh_i, reject_thresh_j). If any pair fails,
 * the model cannot meet the rejection discipline → fail loudly. */
int tpv_cal_check_separability(const tpv_Template *tmpl, int n) {
    if (n < 2) return 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            double d_ij = tpv_cal_mahal_sq(&tmpl[j].mean, (const int32_t *)&tmpl[i].mean, tmpl[i].L_inv);
            double d_ji = tpv_cal_mahal_sq(&tmpl[i].mean, (const int32_t *)&tmpl[j].mean, tmpl[j].L_inv);
            double d2_min = d_ij < d_ji ? d_ij : d_ji;
            int32_t rt_i = tmpl[i].reject_thresh;
            int32_t rt_j = tmpl[j].reject_thresh;
            int32_t rt_max = rt_i > rt_j ? rt_i : rt_j;
            double rt_max_real = rt_max / 65536.0;
            if (d2_min < 2.0 * rt_max_real) {
                fprintf(stderr,
                    "CALIBRATION FAIL: classes %d and %d not separable.\n"
                    "  min squared-Mahalanobis distance between means = %g\n"
                    "  required: > 2 * max(reject_thresh) = %g\n"
                    "  Add features, change product mix, or relax reject_thresh.\n",
                    i, j, d2_min, 2.0 * rt_max_real);
                return -1;
            }
        }
    }
    return 0;
}
