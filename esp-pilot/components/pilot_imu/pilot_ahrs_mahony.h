#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

typedef struct {
    float x;
    float y;
    float z;
} pilot_ahrs_vec3f_t;

typedef struct {
    // Mahony/Complementary quaternion filter gains.
    // Leave as 0 to use defaults.
    float kp; // proportional gain
    float ki; // integral gain (bias estimator), Z bias is not estimated

    // Accel handling.
    float accel_gate_g;     // accept accel update only if | |a|-1 | <= gate
    float accel_lpf_tau_s;  // accel low-pass time constant (s)
} pilot_ahrs_mahony_config_t;

typedef struct {
    // Quaternion (body->nav): q = [w x y z]
    float qw;
    float qx;
    float qy;
    float qz;

    // Gyro bias estimate (rad/s) in body frame, to be SUBTRACTED from gyro measurement.
    pilot_ahrs_vec3f_t bias_rad_s;

    pilot_ahrs_mahony_config_t cfg;
    bool initialized;

    pilot_ahrs_vec3f_t accel_lpf_g;
    bool accel_lpf_initialized;

    // Continuous angles (rad)
    float yaw_last_rad;
    float yaw_total_rad;

    float turn_roll_last_rad;
    float turn_roll_total_rad;

    float turn_pitch_last_rad;
    float turn_pitch_total_rad;
} pilot_ahrs_mahony_t;

void pilot_ahrs_mahony_init(pilot_ahrs_mahony_t *f, const pilot_ahrs_mahony_config_t *cfg);
void pilot_ahrs_mahony_reset(pilot_ahrs_mahony_t *f, float roll_deg, float pitch_deg, float yaw_deg);

void pilot_ahrs_mahony_set_bias_dps(pilot_ahrs_mahony_t *f, pilot_ahrs_vec3f_t bias_dps);
pilot_ahrs_vec3f_t pilot_ahrs_mahony_get_bias_dps(const pilot_ahrs_mahony_t *f);

void pilot_ahrs_mahony_update(pilot_ahrs_mahony_t *f,
                              pilot_ahrs_vec3f_t gyro_dps,
                              pilot_ahrs_vec3f_t accel_g,
                              float dt_s,
                              pilot_ahrs_vec3f_t *out_euler_deg,
                              pilot_ahrs_vec3f_t *out_turn_deg);

#ifdef __cplusplus
}
#endif

