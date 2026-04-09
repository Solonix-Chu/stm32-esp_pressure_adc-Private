#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "mpu6050.h"

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} pilot_imu_raw_axis3_t;

typedef struct {
    float x;
    float y;
    float z;
} pilot_imu_axis3f_t;

typedef struct {
    int64_t timestamp_us;

    pilot_imu_raw_axis3_t raw_accel;
    pilot_imu_raw_axis3_t raw_gyro;

    pilot_imu_axis3f_t accel_g;
    pilot_imu_axis3f_t gyro_dps;
    float temperature_c;

    // Quaternion (body->nav): q = [w x y z]
    float quat_w;
    float quat_x;
    float quat_y;
    float quat_z;

    // Euler angles (ZYX) derived from quaternion for visualization/control.
    // Note: Euler representation has singularities (gimbal lock); prefer quaternion/turn_* for full rotations.
    float roll_deg;
    float pitch_deg;
    float yaw_deg;

    // Continuous turn angles about IMU body axes (unwrapped, can exceed 360°).
    // X/Y are derived from the gravity direction (no [-90,90] pitch limitation); Z is continuous yaw total.
    float turn_roll_deg;
    float turn_pitch_deg;
    float turn_yaw_deg;
} pilot_imu_reading_t;

typedef struct {
    i2c_port_t i2c_port;
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    uint32_t i2c_clk_speed_hz;
    bool i2c_enable_internal_pullups;
    bool i2c_install_driver;

    uint8_t mpu6050_addr; /* 0x68 or 0x69 */
    mpu6050_acce_fs_t accel_fs;
    mpu6050_gyro_fs_t gyro_fs;

    // Attitude filter tuning (Quaternion Mahony/Complementary).
    // Leave as 0 to use defaults.
    float ahrs_kp;              // proportional gain
    float ahrs_ki;              // integral gain (gyro bias estimator, only X/Y)
    float ahrs_accel_gate_g;    // accel magnitude gate around 1g (e.g. 0.2 => [0.8,1.2])
    float ahrs_accel_lpf_tau_s; // accel low-pass time constant (s)

    bool enable_zru;
    float zru_accel_mag_threshold_g;
    float zru_gyro_threshold_dps;
    float zru_learning_rate;
} pilot_imu_config_t;

typedef struct {
    uint32_t sample_count;
    TickType_t inter_sample_delay_ticks;

    bool enable_accel_offset;
    float expected_gravity_g; /* usually 1.0f */
} pilot_imu_calibration_config_t;

typedef struct pilot_imu *pilot_imu_handle_t;

esp_err_t pilot_imu_create(const pilot_imu_config_t *cfg, pilot_imu_handle_t *out_handle);
void pilot_imu_destroy(pilot_imu_handle_t imu);

esp_err_t pilot_imu_calibrate(pilot_imu_handle_t imu, const pilot_imu_calibration_config_t *cfg);

esp_err_t pilot_imu_read_raw(pilot_imu_handle_t imu, pilot_imu_raw_axis3_t *out_accel, pilot_imu_raw_axis3_t *out_gyro);
esp_err_t pilot_imu_update(pilot_imu_handle_t imu, int64_t timestamp_us, pilot_imu_reading_t *out_reading);
esp_err_t pilot_imu_reset_filter(pilot_imu_handle_t imu, float roll_deg, float pitch_deg, float yaw_deg);

#ifdef __cplusplus
}
#endif
