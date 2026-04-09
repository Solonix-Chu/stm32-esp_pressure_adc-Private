/*
 * ESP32 + MPU6050 (I2C) raw read + quaternion attitude (Mahony/complementary) + calibration (gyro bias + optional ZRU)
 */

#include <inttypes.h>

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pilot_imu.h"
#include "pilot_motion.h"

static const char *TAG = "app";

static float cfg_int_to_float(int v, float scale)
{
    return ((float) v) * scale;
}

static esp_err_t i2c_probe_addr(i2c_port_t port, uint8_t addr_7bit)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr_7bit << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static void i2c_scan_once(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, uint32_t clk_hz, bool internal_pullups)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = internal_pullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .scl_pullup_en = internal_pullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .master.clk_speed = clk_hz,
        .clk_flags = 0,
    };

    esp_err_t err = i2c_param_config(port, &conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return;
    }

    err = i2c_driver_install(port, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return;
    }

    bool found_any = false;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_probe_addr(port, addr) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02x", addr);
            found_any = true;
        }
    }
    if (!found_any) {
        ESP_LOGW(TAG, "No I2C devices found (check wiring/pullups/power/pins)");
    }

    (void) i2c_driver_delete(port);
}

#ifdef CONFIG_PILOT_I2C_INTERNAL_PULLUPS
#define PILOT_CFG_I2C_INTERNAL_PULLUPS true
#else
#define PILOT_CFG_I2C_INTERNAL_PULLUPS false
#endif

#ifdef CONFIG_PILOT_IMU_ZRU_ENABLE
#define PILOT_CFG_ZRU_ENABLE true
#define PILOT_CFG_ZRU_ACCEL_MAG_THR_MG CONFIG_PILOT_IMU_ZRU_ACCEL_MAG_THR_MG
#define PILOT_CFG_ZRU_GYRO_THR_MDPS CONFIG_PILOT_IMU_ZRU_GYRO_THR_MDPS
#define PILOT_CFG_ZRU_LEARNING_RATE_PERMILLE CONFIG_PILOT_IMU_ZRU_LEARNING_RATE_PERMILLE
#else
#define PILOT_CFG_ZRU_ENABLE false
#define PILOT_CFG_ZRU_ACCEL_MAG_THR_MG 80
#define PILOT_CFG_ZRU_GYRO_THR_MDPS 2500
#define PILOT_CFG_ZRU_LEARNING_RATE_PERMILLE 0
#endif

#ifdef CONFIG_PILOT_IMU_ACCEL_OFFSET_ENABLE
#define PILOT_CFG_ACCEL_OFFSET_ENABLE true
#else
#define PILOT_CFG_ACCEL_OFFSET_ENABLE false
#endif

void app_main(void)
{
    pilot_imu_handle_t imu = NULL;
    pilot_motion_handle_t motion = NULL;

    const pilot_imu_config_t imu_cfg = {
        .i2c_port = (i2c_port_t) CONFIG_PILOT_I2C_PORT,
        .sda_gpio = (gpio_num_t) CONFIG_PILOT_I2C_SDA_GPIO,
        .scl_gpio = (gpio_num_t) CONFIG_PILOT_I2C_SCL_GPIO,
        .i2c_clk_speed_hz = (uint32_t) CONFIG_PILOT_I2C_CLK_HZ,
        .i2c_enable_internal_pullups = PILOT_CFG_I2C_INTERNAL_PULLUPS,
        .i2c_install_driver = true,

        .mpu6050_addr = (uint8_t) CONFIG_PILOT_MPU6050_ADDR,
        .accel_fs = (mpu6050_acce_fs_t) CONFIG_PILOT_MPU6050_ACCE_FS,
        .gyro_fs = (mpu6050_gyro_fs_t) CONFIG_PILOT_MPU6050_GYRO_FS,

        .enable_zru = PILOT_CFG_ZRU_ENABLE,
        .zru_accel_mag_threshold_g = cfg_int_to_float(PILOT_CFG_ZRU_ACCEL_MAG_THR_MG, 0.001f),
        .zru_gyro_threshold_dps = cfg_int_to_float(PILOT_CFG_ZRU_GYRO_THR_MDPS, 0.001f),
        .zru_learning_rate = cfg_int_to_float(PILOT_CFG_ZRU_LEARNING_RATE_PERMILLE, 0.001f),
    };

    while (1) {
        const esp_err_t err = pilot_imu_create(&imu_cfg, &imu);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG,
                 "pilot_imu_create failed: %s (port=%d sda=%d scl=%d clk=%" PRIu32 " addr=0x%02x). Retrying...",
                 esp_err_to_name(err),
                 (int) imu_cfg.i2c_port,
                 (int) imu_cfg.sda_gpio,
                 (int) imu_cfg.scl_gpio,
                 imu_cfg.i2c_clk_speed_hz,
                 imu_cfg.mpu6050_addr);
        i2c_scan_once(imu_cfg.i2c_port,
                      imu_cfg.sda_gpio,
                      imu_cfg.scl_gpio,
                      imu_cfg.i2c_clk_speed_hz,
                      imu_cfg.i2c_enable_internal_pullups);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    const pilot_imu_calibration_config_t cal_cfg = {
        .sample_count = CONFIG_PILOT_IMU_CAL_SAMPLES,
        .inter_sample_delay_ticks = pdMS_TO_TICKS(CONFIG_PILOT_IMU_CAL_SAMPLE_DELAY_MS),
        .enable_accel_offset = PILOT_CFG_ACCEL_OFFSET_ENABLE,
        .expected_gravity_g = 1.0f,
    };

    ESP_LOGI(TAG, "Calibrating... keep device still");
    ESP_ERROR_CHECK(pilot_imu_calibrate(imu, &cal_cfg));

    const pilot_motion_config_t motion_cfg = {
        .sample_rate_hz = (float) CONFIG_PILOT_IMU_RATE_HZ,
        .window_s = 1.0f,
        .min_freq_hz = 0.5f,
        .max_freq_hz = 12.0f,
        .shake_peak_abs_threshold_g = 0.05f,
        .rot_peak_abs_threshold_dps = 5.0f,
        .peak_threshold_sigma = 0.8f,
        .gravity_g = 1.0f,
    };
    ESP_ERROR_CHECK(pilot_motion_create(&motion_cfg, &motion));

    // const TickType_t loop_delay = pdMS_TO_TICKS(1000 / CONFIG_PILOT_IMU_RATE_HZ);
    const TickType_t loop_delay = pdMS_TO_TICKS(1000 / CONFIG_PILOT_IMU_RATE_HZ);
    while (1) {
        pilot_imu_reading_t r = {0};
        const int64_t now_us = esp_timer_get_time();
        esp_err_t err = pilot_imu_update(imu, now_us, &r);
        if (err == ESP_OK) {
            pilot_motion_reading_t m = {0};
            (void) pilot_motion_update(motion, &r, &m);
            ESP_LOGI(TAG,
                     "raw_a=[%d %d %d] raw_g=[%d %d %d] "
                     "a_g=[%.3f %.3f %.3f] g_dps=[%.3f %.3f %.3f] "
                     "a_nav_g=[%.3f %.3f %.3f] lin_a_nav_g=[%.3f %.3f %.3f] gyro_nav_dps=[%.3f %.3f %.3f] "
                     "shake=[%.3f %.3f %.2f] rot=[%.3f %.3f %.2f] "
                     "quat=[%.6f %.6f %.6f %.6f] euler_deg=[%.2f %.2f %.2f] "
                     "turn_deg=[%.2f %.2f %.2f] t=%.1fC ts=%" PRId64,
                     r.raw_accel.x, r.raw_accel.y, r.raw_accel.z,
                     r.raw_gyro.x, r.raw_gyro.y, r.raw_gyro.z,
                     r.accel_g.x, r.accel_g.y, r.accel_g.z,
                     r.gyro_dps.x, r.gyro_dps.y, r.gyro_dps.z,
                     m.accel_nav_g.x, m.accel_nav_g.y, m.accel_nav_g.z,
                     m.lin_accel_nav_g.x, m.lin_accel_nav_g.y, m.lin_accel_nav_g.z,
                     m.gyro_nav_dps.x, m.gyro_nav_dps.y, m.gyro_nav_dps.z,
                     m.shake_rms_g, m.shake_peak_g, m.shake_freq_hz,
                     m.rot_rms_dps, m.rot_peak_dps, m.rot_freq_hz,
                     r.quat_w, r.quat_x, r.quat_y, r.quat_z,
                     r.roll_deg, r.pitch_deg, r.yaw_deg,
                     r.turn_roll_deg, r.turn_pitch_deg, r.turn_yaw_deg,
                     r.temperature_c, r.timestamp_us);
        } else {
            ESP_LOGW(TAG, "imu read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(loop_delay);
    }
}
