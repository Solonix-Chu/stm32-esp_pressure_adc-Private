#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "esp_err.h"

#include "pilot_imu.h"

typedef struct {
    // Required: nominal sample rate used for window sizing.
    float sample_rate_hz;

    // Sliding window length (seconds) used for amplitude/frequency estimation.
    // 0 => default (2.0s).
    float window_s;

    // Frequency clamp (Hz) for peak-based estimator. 0 disables the clamp.
    float min_freq_hz;
    float max_freq_hz;

    // Peak detection thresholds.
    // A peak must exceed max(abs_threshold, sigma*std) to be counted.
    float shake_peak_abs_threshold_g;
    float rot_peak_abs_threshold_dps;
    float peak_threshold_sigma;

    // Gravity magnitude (g) in nav frame (+Z direction). 0 => default (1.0g).
    float gravity_g;
} pilot_motion_config_t;

typedef struct {
    // World/nav frame vectors derived from quaternion (body->nav).
    pilot_imu_axis3f_t accel_nav_g;     // includes gravity
    pilot_imu_axis3f_t lin_accel_nav_g; // gravity removed
    pilot_imu_axis3f_t gyro_nav_dps;    // bias-corrected angular rate in nav

    float lin_accel_mag_g;
    float gyro_mag_dps;

    // Shake (translation) metrics from lin_accel_nav_g over sliding window.
    float shake_rms_g;
    float shake_peak_g;
    float shake_freq_hz;

    // Rotation metrics from gyro_nav_dps over sliding window.
    float rot_rms_dps;
    float rot_peak_dps;
    float rot_freq_hz;
} pilot_motion_reading_t;

typedef struct pilot_motion *pilot_motion_handle_t;

esp_err_t pilot_motion_create(const pilot_motion_config_t *cfg, pilot_motion_handle_t *out_handle);
void pilot_motion_destroy(pilot_motion_handle_t motion);

void pilot_motion_reset(pilot_motion_handle_t motion);

esp_err_t pilot_motion_update(pilot_motion_handle_t motion,
                              const pilot_imu_reading_t *imu,
                              pilot_motion_reading_t *out_motion);

#ifdef __cplusplus
}
#endif

