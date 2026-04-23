#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tpv_internal.h"

/* Calibration tool API (in stats.c, separability.c, codegen.c, frame_io.c) */
int     tpv_cal_load_class_frames(const char *dir, tpv_Features *out, int cap);
uint8_t tpv_cal_estimate_bin_threshold(const char *const *dirs, int n_dirs);
void    tpv_cal_mean_cov(const tpv_Features *s, int n, int32_t *mean, double *cov);
void    tpv_cal_regularize(double *cov, const double *sigma_ref_sq);
int     tpv_cal_cholesky_inv(const double *cov, int32_t *L_inv_q16);
int     tpv_cal_quantize_or_fail(double real, int32_t *q16_out, const char *label);
double  tpv_cal_mahal_sq(const tpv_Features *x, const int32_t *mean_q16,
                         const int32_t *L_inv_q16);
int     tpv_cal_check_separability(const tpv_Template *tmpl, int n);
int     tpv_cal_emit_model(const tpv_Template *t, int n, uint8_t bin_thresh, FILE *out);

#define MAX_CLASSES               5
#define MAX_SAMPLES_PER_CLASS   256

/* Compute σ²_ref per dimension as the squared range of all training samples
 * across all classes. Used by Tikhonov regularization (spec §8 step 4). */
static void compute_sigma_ref_sq(tpv_Features samples[][MAX_SAMPLES_PER_CLASS],
                                 int *nsamp, int n_classes,
                                 double *sigma_ref_sq) {
    for (int k = 0; k < TPV_N_FEAT; k++) {
        double lo =  1e300, hi = -1e300;
        for (int c = 0; c < n_classes; c++) {
            for (int i = 0; i < nsamp[c]; i++) {
                const int32_t *v = (const int32_t *)&samples[c][i];
                double real = v[k] / 65536.0;
                if (real < lo) lo = real;
                if (real > hi) hi = real;
            }
        }
        double range = (hi > lo) ? (hi - lo) : 1.0;
        sigma_ref_sq[k] = range * range;
    }
}

int main(int argc, char **argv) {
    const char *out_path = "model_data.c";
    int n_classes = 0;
    const char *class_dirs[MAX_CLASSES];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out_path = argv[++i]; continue; }
        if (n_classes >= MAX_CLASSES) {
            fprintf(stderr, "too many classes (max %d)\n", MAX_CLASSES); return 2;
        }
        class_dirs[n_classes++] = argv[i];
    }
    if (n_classes < 1) {
        fprintf(stderr, "usage: calibrate DIR1 [DIR2 ...] [-o out_path]\n");
        return 2;
    }

    /* 1a. Estimate the binary threshold via Otsu over a histogram sampled from
     *     all class directories. Writes into the runtime extern so step 1b's
     *     feature extraction uses the calibrated value (not the 128 default). */
    uint8_t bin_thresh = tpv_cal_estimate_bin_threshold(class_dirs, n_classes);
    fprintf(stderr, "calibrate: Otsu bin_threshold = %u\n", bin_thresh);

    /* 1b. Load samples per class. */
    static tpv_Features samples[MAX_CLASSES][MAX_SAMPLES_PER_CLASS];
    int nsamp[MAX_CLASSES] = {0};
    for (int c = 0; c < n_classes; c++) {
        nsamp[c] = tpv_cal_load_class_frames(class_dirs[c], samples[c], MAX_SAMPLES_PER_CLASS);
        if (nsamp[c] < 10) {
            fprintf(stderr, "class %d (%s) has only %d samples; need ≥ 10\n",
                    c, class_dirs[c], nsamp[c]);
            return 1;
        }
    }

    /* 2. Mean + covariance + regularize + Cholesky per class */
    tpv_Template tmpl[MAX_CLASSES] = {0};
    static double covs[MAX_CLASSES][TPV_N_FEAT * TPV_N_FEAT];
    double sigma_ref_sq[TPV_N_FEAT];
    compute_sigma_ref_sq(samples, nsamp, n_classes, sigma_ref_sq);
    for (int c = 0; c < n_classes; c++) {
        int32_t mean[TPV_N_FEAT];
        tpv_cal_mean_cov(samples[c], nsamp[c], mean, covs[c]);
        tpv_cal_regularize(covs[c], sigma_ref_sq);
        memcpy(&tmpl[c].mean, mean, sizeof mean);
        if (tpv_cal_cholesky_inv(covs[c], tmpl[c].L_inv) < 0) {
            fprintf(stderr, "class %d covariance not positive definite after regularization\n", c);
            return 1;
        }
    }

    /* 3. reject_thresh_c = 1.5 × max intra-class squared Mahalanobis */
    for (int c = 0; c < n_classes; c++) {
        double max_d2 = 0;
        for (int i = 0; i < nsamp[c]; i++) {
            double d2 = tpv_cal_mahal_sq(&samples[c][i],
                                         (const int32_t *)&tmpl[c].mean,
                                         tmpl[c].L_inv);
            if (d2 > max_d2) max_d2 = d2;
        }
        if (tpv_cal_quantize_or_fail(max_d2 * 1.5, &tmpl[c].reject_thresh, "reject_thresh") < 0)
            return 1;
    }

    /* 4. margin_c = 0.25 × min inter-class squared Mahalanobis (skip when N=1) */
    if (n_classes >= 2) {
        for (int c = 0; c < n_classes; c++) {
            double min_inter_d2 = 1e30;
            for (int cp = 0; cp < n_classes; cp++) {
                if (cp == c) continue;
                double d2 = tpv_cal_mahal_sq(&tmpl[cp].mean,
                                             (const int32_t *)&tmpl[c].mean,
                                             tmpl[c].L_inv);
                if (d2 < min_inter_d2) min_inter_d2 = d2;
            }
            if (tpv_cal_quantize_or_fail(min_inter_d2 * 0.25, &tmpl[c].margin, "margin") < 0)
                return 1;
        }
    } else {
        tmpl[0].margin = 0;     /* spec §8 step 7: legal for single-class */
    }

    /* 5. Pairwise separability sanity check */
    if (tpv_cal_check_separability(tmpl, n_classes) < 0) return 1;

    /* 6. Emit model_data.c (carrying the Otsu-estimated bin_thresh from 1a). */
    FILE *out = fopen(out_path, "w");
    if (!out) { perror("open out"); return 1; }
    tpv_cal_emit_model(tmpl, n_classes, bin_thresh, out);
    fclose(out);
    fprintf(stderr, "calibrate: wrote %s for %d class(es)\n", out_path, n_classes);
    return 0;
}
