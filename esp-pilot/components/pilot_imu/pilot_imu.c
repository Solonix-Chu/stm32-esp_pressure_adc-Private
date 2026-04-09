#include "pilot_imu.h"

#include <stdlib.h>
#include <math.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pilot_ahrs_mahony.h"

#define RAD_TO_DEG (57.29577951308232f)
#define DEG_TO_RAD (0.017453292519943295f)

struct pilot_imu {
    pilot_imu_config_t cfg;
    mpu6050_handle_t mpu;
    bool i2c_installed;

    pilot_ahrs_mahony_t ahrs;
    pilot_imu_axis3f_t accel_offset_g;

    int64_t last_ts_us;
};

static const char *TAG = "pilot_imu";

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float wrap180(float a_deg)
{
    while (a_deg > 180.0f) a_deg -= 360.0f;
    while (a_deg < -180.0f) a_deg += 360.0f;
    return a_deg;
}

static float accel_roll_deg(const pilot_imu_axis3f_t *a_g)
{
    return atan2f(a_g->y, a_g->z) * RAD_TO_DEG;
}

static float accel_pitch_deg(const pilot_imu_axis3f_t *a_g)
{
    const float denom = sqrtf(a_g->y * a_g->y + a_g->z * a_g->z);
    return atan2f(-a_g->x, denom) * RAD_TO_DEG;
}

static esp_err_t ensure_i2c(const pilot_imu_config_t *cfg, bool *out_installed)
{
    if (!cfg || !out_installed) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!cfg->i2c_install_driver) {
        *out_installed = false;
        return ESP_OK;
    }

    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .sda_pullup_en = cfg->i2c_enable_internal_pullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .scl_pullup_en = cfg->i2c_enable_internal_pullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .master.clk_speed = cfg->i2c_clk_speed_hz,
        .clk_flags = 0,
    };

    esp_err_t err = i2c_param_config(cfg->i2c_port, &i2c_conf);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_driver_install(cfg->i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (err == ESP_OK) {
        *out_installed = true;
    }
    return err;
}

esp_err_t pilot_imu_create(const pilot_imu_config_t *cfg, pilot_imu_handle_t *out_handle)
{
    if (!cfg || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->mpu6050_addr != 0x68 && cfg->mpu6050_addr != 0x69) {
        return ESP_ERR_INVALID_ARG;
    }

    pilot_imu_handle_t imu = calloc(1, sizeof(*imu));
    if (!imu) {
        return ESP_ERR_NO_MEM;
    }
    imu->cfg = *cfg;

    esp_err_t err = ensure_i2c(cfg, &imu->i2c_installed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s (port=%d sda=%d scl=%d clk=%" PRIu32 ")",
                 esp_err_to_name(err),
                 (int) cfg->i2c_port,
                 (int) cfg->sda_gpio,
                 (int) cfg->scl_gpio,
                 cfg->i2c_clk_speed_hz);
        free(imu);
        return err;
    }

    imu->mpu = mpu6050_create(cfg->i2c_port, cfg->mpu6050_addr);
    if (!imu->mpu) {
        ESP_LOGE(TAG, "mpu6050_create failed (out of memory?)");
        free(imu);
        return ESP_FAIL;
    }

    uint8_t device_id = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        err = mpu6050_get_deviceid(imu->mpu, &device_id);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s (addr=0x%02x)", esp_err_to_name(err), cfg->mpu6050_addr);
        pilot_imu_destroy(imu);
        return err;
    }
    if (device_id != MPU6050_WHO_AM_I_VAL) {
        ESP_LOGW(TAG, "Unexpected WHO_AM_I=0x%02x (expect 0x%02x)", device_id, MPU6050_WHO_AM_I_VAL);
    }

    err = mpu6050_wake_up(imu->mpu);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mpu6050_wake_up failed: %s", esp_err_to_name(err));
        pilot_imu_destroy(imu);
        return err;
    }

    err = mpu6050_config(imu->mpu, cfg->accel_fs, cfg->gyro_fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mpu6050_config failed: %s", esp_err_to_name(err));
        pilot_imu_destroy(imu);
        return err;
    }

    const pilot_ahrs_mahony_config_t ahrs_cfg = {
        .kp = cfg->ahrs_kp,
        .ki = cfg->ahrs_ki,
        .accel_gate_g = cfg->ahrs_accel_gate_g,
        .accel_lpf_tau_s = cfg->ahrs_accel_lpf_tau_s,
    };
    pilot_ahrs_mahony_init(&imu->ahrs, &ahrs_cfg);

    imu->last_ts_us = 0;

    imu->accel_offset_g = (pilot_imu_axis3f_t) {0};

    *out_handle = imu;
    return ESP_OK;
}

void pilot_imu_destroy(pilot_imu_handle_t imu)
{
    if (!imu) {
        return;
    }

    if (imu->mpu) {
        mpu6050_delete(imu->mpu);
        imu->mpu = NULL;
    }
    if (imu->i2c_installed) {
        i2c_driver_delete(imu->cfg.i2c_port);
        imu->i2c_installed = false;
    }
    free(imu);
}

esp_err_t pilot_imu_reset_filter(pilot_imu_handle_t imu, float roll_deg, float pitch_deg, float yaw_deg)
{
    if (!imu) {
        return ESP_ERR_INVALID_ARG;
    }

    pilot_ahrs_mahony_reset(&imu->ahrs, roll_deg, pitch_deg, yaw_deg);

    imu->last_ts_us = 0;
    return ESP_OK;
}

esp_err_t pilot_imu_read_raw(pilot_imu_handle_t imu, pilot_imu_raw_axis3_t *out_accel, pilot_imu_raw_axis3_t *out_gyro)
{
    if (!imu || !out_accel || !out_gyro) {
        return ESP_ERR_INVALID_ARG;
    }

    mpu6050_raw_acce_value_t ra = {0};
    mpu6050_raw_gyro_value_t rg = {0};

    esp_err_t err = mpu6050_get_raw_acce(imu->mpu, &ra);
    if (err != ESP_OK) {
        return err;
    }
    err = mpu6050_get_raw_gyro(imu->mpu, &rg);
    if (err != ESP_OK) {
        return err;
    }

    *out_accel = (pilot_imu_raw_axis3_t) {ra.raw_acce_x, ra.raw_acce_y, ra.raw_acce_z};
    *out_gyro = (pilot_imu_raw_axis3_t) {rg.raw_gyro_x, rg.raw_gyro_y, rg.raw_gyro_z};
    return ESP_OK;
}

static bool is_stationary(const pilot_imu_config_t *cfg,
                          const pilot_imu_axis3f_t *accel_g,
                          const pilot_imu_axis3f_t *gyro_meas_dps,
                          const pilot_imu_axis3f_t *gyro_bias_dps)
{
    if (!cfg->enable_zru) {
        return false;
    }

    const pilot_imu_axis3f_t gyro_corr = {
        gyro_meas_dps->x - gyro_bias_dps->x,
        gyro_meas_dps->y - gyro_bias_dps->y,
        gyro_meas_dps->z - gyro_bias_dps->z,
    };
    const float gyro_abs_max = fmaxf(fabsf(gyro_corr.x), fmaxf(fabsf(gyro_corr.y), fabsf(gyro_corr.z)));
    if (gyro_abs_max > cfg->zru_gyro_threshold_dps) {
        return false;
    }

    const float mag = sqrtf(accel_g->x * accel_g->x + accel_g->y * accel_g->y + accel_g->z * accel_g->z);
    return fabsf(mag - 1.0f) <= cfg->zru_accel_mag_threshold_g;
}

static void zru_update_bias(pilot_imu_handle_t imu, const pilot_imu_axis3f_t *gyro_meas_dps)
{
    const float k = clampf(imu->cfg.zru_learning_rate, 0.0f, 1.0f);

    pilot_ahrs_vec3f_t bias = pilot_ahrs_mahony_get_bias_dps(&imu->ahrs);
    bias.x = bias.x * (1.0f - k) + gyro_meas_dps->x * k;
    bias.y = bias.y * (1.0f - k) + gyro_meas_dps->y * k;
    bias.z = bias.z * (1.0f - k) + gyro_meas_dps->z * k;
    pilot_ahrs_mahony_set_bias_dps(&imu->ahrs, bias);
}

esp_err_t pilot_imu_update(pilot_imu_handle_t imu, int64_t timestamp_us, pilot_imu_reading_t *out_reading)
{
    if (!imu || !out_reading) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timestamp_us <= 0) {
        timestamp_us = esp_timer_get_time();
    }

    mpu6050_raw_acce_value_t ra = {0};
    mpu6050_raw_gyro_value_t rg = {0};
    mpu6050_acce_value_t a = {0};
    mpu6050_gyro_value_t g = {0};
    mpu6050_temp_value_t t = {0};

    esp_err_t err = mpu6050_get_raw_acce(imu->mpu, &ra);
    if (err != ESP_OK) {
        return err;
    }
    err = mpu6050_get_raw_gyro(imu->mpu, &rg);
    if (err != ESP_OK) {
        return err;
    }
    err = mpu6050_get_acce(imu->mpu, &a);
    if (err != ESP_OK) {
        return err;
    }
    err = mpu6050_get_gyro(imu->mpu, &g);
    if (err != ESP_OK) {
        return err;
    }
    (void) mpu6050_get_temp(imu->mpu, &t);

    pilot_imu_axis3f_t accel_g = {a.acce_x - imu->accel_offset_g.x, a.acce_y - imu->accel_offset_g.y, a.acce_z - imu->accel_offset_g.z};
    const pilot_imu_axis3f_t gyro_meas_dps = {g.gyro_x, g.gyro_y, g.gyro_z};
    const pilot_ahrs_vec3f_t bias_dps0 = pilot_ahrs_mahony_get_bias_dps(&imu->ahrs);
    const pilot_imu_axis3f_t bias0 = {bias_dps0.x, bias_dps0.y, bias_dps0.z};
    if (is_stationary(&imu->cfg, &accel_g, &gyro_meas_dps, &bias0)) {
        zru_update_bias(imu, &gyro_meas_dps);
    }

    float dt_s = 0.0f;
    if (imu->last_ts_us != 0) {
        dt_s = (timestamp_us - imu->last_ts_us) / 1000000.0f;
        dt_s = clampf(dt_s, 0.0005f, 0.1f);
    }
    imu->last_ts_us = timestamp_us;

    pilot_ahrs_vec3f_t euler_deg = {0};
    pilot_ahrs_vec3f_t turn_deg = {0};
    pilot_ahrs_mahony_update(&imu->ahrs,
                             (pilot_ahrs_vec3f_t) {gyro_meas_dps.x, gyro_meas_dps.y, gyro_meas_dps.z},
                             (pilot_ahrs_vec3f_t) {accel_g.x, accel_g.y, accel_g.z},
                             dt_s,
                             &euler_deg,
                             &turn_deg);

    const pilot_ahrs_vec3f_t bias_dps = pilot_ahrs_mahony_get_bias_dps(&imu->ahrs);
    pilot_imu_axis3f_t gyro_dps = {
        gyro_meas_dps.x - bias_dps.x,
        gyro_meas_dps.y - bias_dps.y,
        gyro_meas_dps.z - bias_dps.z,
    };

    *out_reading = (pilot_imu_reading_t) {
        .timestamp_us = timestamp_us,
        .raw_accel = {ra.raw_acce_x, ra.raw_acce_y, ra.raw_acce_z},
        .raw_gyro = {rg.raw_gyro_x, rg.raw_gyro_y, rg.raw_gyro_z},
        .accel_g = accel_g,
        .gyro_dps = gyro_dps,
        .temperature_c = t.temp,
        .quat_w = imu->ahrs.qw,
        .quat_x = imu->ahrs.qx,
        .quat_y = imu->ahrs.qy,
        .quat_z = imu->ahrs.qz,
        .roll_deg = wrap180(euler_deg.x),
        .pitch_deg = wrap180(euler_deg.y),
        .yaw_deg = wrap180(euler_deg.z),

        .turn_roll_deg = turn_deg.x,
        .turn_pitch_deg = turn_deg.y,
        .turn_yaw_deg = turn_deg.z,
    };

    return ESP_OK;
}

esp_err_t pilot_imu_calibrate(pilot_imu_handle_t imu, const pilot_imu_calibration_config_t *cfg)
{
    if (!imu || !cfg || cfg->sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    pilot_imu_axis3f_t gyro_sum = {0};
    pilot_imu_axis3f_t accel_sum = {0};

    for (uint32_t i = 0; i < cfg->sample_count; i++) {
        mpu6050_acce_value_t a = {0};
        mpu6050_gyro_value_t g = {0};

        esp_err_t err = mpu6050_get_acce(imu->mpu, &a);
        if (err != ESP_OK) {
            return err;
        }
        err = mpu6050_get_gyro(imu->mpu, &g);
        if (err != ESP_OK) {
            return err;
        }

        accel_sum.x += a.acce_x;
        accel_sum.y += a.acce_y;
        accel_sum.z += a.acce_z;

        gyro_sum.x += g.gyro_x;
        gyro_sum.y += g.gyro_y;
        gyro_sum.z += g.gyro_z;

        if (cfg->inter_sample_delay_ticks > 0) {
            vTaskDelay(cfg->inter_sample_delay_ticks);
        }
    }

    const float inv = 1.0f / (float) cfg->sample_count;
    const pilot_imu_axis3f_t accel_avg = {accel_sum.x * inv, accel_sum.y * inv, accel_sum.z * inv};
    const pilot_imu_axis3f_t gyro_avg = {gyro_sum.x * inv, gyro_sum.y * inv, gyro_sum.z * inv};

    pilot_ahrs_mahony_set_bias_dps(&imu->ahrs, (pilot_ahrs_vec3f_t) {gyro_avg.x, gyro_avg.y, gyro_avg.z});

    if (cfg->enable_accel_offset) {
        const float gexp = (cfg->expected_gravity_g > 0.0f) ? cfg->expected_gravity_g : 1.0f;
        imu->accel_offset_g.x = accel_avg.x;
        imu->accel_offset_g.y = accel_avg.y;
        imu->accel_offset_g.z = accel_avg.z - gexp;
    }

    const float roll0 = accel_roll_deg(&accel_avg);
    const float pitch0 = accel_pitch_deg(&accel_avg);
    pilot_imu_reset_filter(imu, roll0, pitch0, 0.0f);

    ESP_LOGI(TAG, "Calibration done: gyro_bias_dps=[%.3f %.3f %.3f] roll0=%.2f pitch0=%.2f",
             gyro_avg.x, gyro_avg.y, gyro_avg.z, roll0, pitch0);
    return ESP_OK;
}
