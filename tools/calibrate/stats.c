#include <string.h>
#include "tpv_internal.h"

/* Compute Q16.16 mean and double covariance (in real units) from N feature
 * samples. cov is row-major TPV_N_FEAT × TPV_N_FEAT. */
void tpv_cal_mean_cov(const tpv_Features *samples, int n,
                      int32_t *mean_out, double *cov_out) {
    double mean_real[TPV_N_FEAT] = {0};
    for (int i = 0; i < n; i++) {
        const int32_t *v = (const int32_t *)&samples[i];
        for (int k = 0; k < TPV_N_FEAT; k++)
            mean_real[k] += v[k] / 65536.0;
    }
    for (int k = 0; k < TPV_N_FEAT; k++) {
        mean_real[k] /= n;
        mean_out[k] = (int32_t)(mean_real[k] * 65536);
    }
    for (int i = 0; i < TPV_N_FEAT * TPV_N_FEAT; i++) cov_out[i] = 0;
    for (int s = 0; s < n; s++) {
        const int32_t *v = (const int32_t *)&samples[s];
        double d[TPV_N_FEAT];
        for (int k = 0; k < TPV_N_FEAT; k++) d[k] = v[k]/65536.0 - mean_real[k];
        for (int i = 0; i < TPV_N_FEAT; i++)
            for (int j = 0; j <= i; j++)
                cov_out[i*TPV_N_FEAT+j] += d[i] * d[j];
    }
    double norm = (n > 1) ? (double)(n - 1) : 1.0;
    for (int i = 0; i < TPV_N_FEAT; i++)
        for (int j = 0; j <= i; j++) {
            cov_out[i*TPV_N_FEAT+j] /= norm;
            cov_out[j*TPV_N_FEAT+i]  = cov_out[i*TPV_N_FEAT+j];   /* symmetry */
        }
}

/* Tikhonov regularization: Σ ← Σ + ε · diag(σ²_ref). Spec §8 step 4 — handles
 * degenerate dimensions (e.g. m3_axis_sign for symmetric classes). */
void tpv_cal_regularize(double *cov, const double *sigma_ref_sq) {
    const double eps = 1e-4;
    for (int i = 0; i < TPV_N_FEAT; i++)
        cov[i*TPV_N_FEAT+i] += eps * sigma_ref_sq[i];
}

/* In-place Cholesky: Σ = L Lᵀ, then invert L (lower triangular), pack as
 * Q16.16 row-major. Returns -1 if Σ is not positive definite. */
int tpv_cal_cholesky_inv(const double *cov, int32_t *L_inv_q16_out) {
    double L[TPV_N_FEAT][TPV_N_FEAT] = {{0}};
    for (int i = 0; i < TPV_N_FEAT; i++) {
        for (int j = 0; j <= i; j++) {
            double sum = cov[i*TPV_N_FEAT+j];
            for (int k = 0; k < j; k++) sum -= L[i][k] * L[j][k];
            if (i == j) {
                if (sum <= 0) return -1;
                L[i][i] = __builtin_sqrt(sum);
            } else {
                L[i][j] = sum / L[j][j];
            }
        }
    }
    double Linv[TPV_N_FEAT][TPV_N_FEAT] = {{0}};
    for (int i = 0; i < TPV_N_FEAT; i++) {
        Linv[i][i] = 1.0 / L[i][i];
        for (int j = 0; j < i; j++) {
            double s = 0;
            for (int k = j; k < i; k++) s -= L[i][k] * Linv[k][j];
            Linv[i][j] = s / L[i][i];
        }
    }
    int idx = 0;
    for (int i = 0; i < TPV_N_FEAT; i++)
        for (int j = 0; j <= i; j++)
            L_inv_q16_out[idx++] = (int32_t)(Linv[i][j] * 65536.0);
    return 0;
}

/* Squared Mahalanobis distance evaluated host-side in double precision.
 * Mirrors the runtime mahal_sq_q16 logic in src/classifier.c. Used by
 * calibrate.c to derive reject_thresh and margin from training samples. */
double tpv_cal_mahal_sq(const tpv_Features *x, const int32_t *mean_q16,
                        const int32_t *L_inv_q16) {
    double dx[TPV_N_FEAT];
    const int32_t *xv = (const int32_t *)x;
    for (int i = 0; i < TPV_N_FEAT; i++)
        dx[i] = (xv[i] - mean_q16[i]) / 65536.0;

    double y2 = 0;
    int idx = 0;
    for (int i = 0; i < TPV_N_FEAT; i++) {
        double yi = 0;
        for (int j = 0; j <= i; j++)
            yi += (L_inv_q16[idx + j] / 65536.0) * dx[j];
        idx += (i + 1);
        y2 += yi * yi;
    }
    return y2;
}

#include <stdio.h>

/* Q16.16 quantization with HG3 floor: real values that would quantize below
 * 1 LSB cause calibration to fail loudly with an actionable diagnostic. */
int tpv_cal_quantize_or_fail(double real, int32_t *q16_out, const char *label) {
    double q = real * 65536.0;
    if (q < 1.0) {
        fprintf(stderr,
            "CALIBRATION FAIL: %s quantizes to 0 in Q16.16 (real=%g).\n"
            "  Likely cause: training variance too small, near-identical samples,\n"
            "  or log-Hu numerical collapse. Inspect raw feature distribution.\n",
            label, real);
        return -1;
    }
    *q16_out = (int32_t)q;
    return 0;
}
