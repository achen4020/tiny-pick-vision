#include "tpv_internal.h"
#include "testlib.h"

TEST(t_centroid_of_square) {
    tpv_Blob b = {0};
    b.m00 = 16; b.m10 = 24; b.m01 = 24;       /* centroid (1.5, 1.5) */
    b.mu20 = 20; b.mu02 = 20; b.mu11 = 0;
    int16_t x, y, th;
    tpv_pose(&b, &x, &y, &th);
    CHECK_EQ_I(x, 1);          /* truncated centroid */
    CHECK_EQ_I(y, 1);
    CHECK_EQ_I(th, 0);          /* μ₂₀ == μ₀₂ + μ₁₁ == 0 → arctan(0,0) = 0 */
}

TEST(t_axis_angle_of_horizontal_bar) {
    /* Wide rectangle: μ₂₀ ≫ μ₀₂, μ₁₁ = 0 → principal axis horizontal, θ ≈ 0. */
    tpv_Blob b = {0};
    b.m00 = 40; b.m10 = 100; b.m01 = 80;
    b.mu20 = 200; b.mu02 = 10; b.mu11 = 0;
    int16_t x, y, th;
    tpv_pose(&b, &x, &y, &th);
    CHECK(th > -5 && th < 5);   /* allow ±0.5° */
}

TEST(t_axis_angle_of_vertical_bar_is_in_range) {
    /* Tall rectangle: μ₀₂ ≫ μ₂₀, μ₁₁ = 0 → principal axis vertical,
     * θ ≈ ±π/2. Contract: theta_x10 ∈ [-900, 900). Without the clamp,
     * a rounding change in atan2_q16 could push this to exactly +900,
     * which is out of range. */
    tpv_Blob b = {0};
    b.m00 = 40; b.m10 = 80; b.m01 = 100;
    b.mu20 = 10; b.mu02 = 200; b.mu11 = 0;
    int16_t x, y, th;
    tpv_pose(&b, &x, &y, &th);
    CHECK(th >= -900 && th < 900);
    /* And the angle should be near ±π/2 (i.e., ±900 boundary, allow ±10). */
    CHECK((th >= 890 && th < 900) || (th >= -900 && th <= -890));
}

int main(void) {
    RUN(t_centroid_of_square);
    RUN(t_axis_angle_of_horizontal_bar);
    RUN(t_axis_angle_of_vertical_bar_is_in_range);
    FINISH();
}
