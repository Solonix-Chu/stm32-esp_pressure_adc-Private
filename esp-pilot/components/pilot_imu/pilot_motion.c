#include "pilot_motion.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define US_PER_S (1000000LL)

struct pilot_motion {
    pilot_motion_config_t cfg;

    size_t cap;
    size_t count;
    size_t head; // next write index

    int64_t *ts_us;
    float *lin_x;
    float *lin_y;
    float *lin_z;
    float *gyro_x;
    float *gyro_y;
    float *gyro_z;

    int64_t last_ts_us;
};

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float safe_sqrtf(float v)
{
    return sqrtf(fmaxf(v, 0.0f));
}

static size_t ring_oldest_index(const pilot_motion_handle_t m)
{
    return (m->head + m->cap - m->count) % m->cap;
}

static size_t ring_index_at(const pilot_motion_handle_t m, size_t i)
{
    // i: 0..count-1 (oldest->newest)
    const size_t start = ring_oldest_index(m);
    return (start + i) % m->cap;
}

static pilot_imu_axis3f_t quat_rotate_body_to_nav(float qw, float qx, float qy, float qz, pilot_imu_axis3f_t v)
{
    // v' = q * v * q_conj, optimized (assumes q is unit).
    const float vx = v.x;
    const float vy = v.y;
    const float vz = v.z;

    const float tx = 2.0f * (qy * vz - qz * vy);
    const float ty = 2.0f * (qz * vx - qx * vz);
    const float tz = 2.0f * (qx * vy - qy * vx);

    return (pilot_imu_axis3f_t) {
        vx + qw * tx + (qy * tz - qz * ty),
        vy + qw * ty + (qz * tx - qx * tz),
        vz + qw * tz + (qx * ty - qy * tx),
    };
}

static float estimate_freq_peaks_axis(const pilot_motion_handle_t m,
                                      const float *axis,
                                      float mean,
                                      float threshold,
                                      int64_t min_period_us,
                                      int64_t max_period_us)
{
    if (!m || m->count < 5) {
        return 0.0f;
    }

    int64_t last_peak_us = INT64_MIN;
    int64_t sum_period_us = 0;
    int periods = 0;

    for (size_t i = 1; i + 1 < m->count; i++) {
        const size_t idx0 = ring_index_at(m, i - 1);
        const size_t idx1 = ring_index_at(m, i);
        const size_t idx2 = ring_index_at(m, i + 1);

        const float x0 = axis[idx0] - mean;
        const float x1 = axis[idx1] - mean;
        const float x2 = axis[idx2] - mean;

        const bool is_peak = (x1 > x0) && (x1 >= x2) && (x1 > threshold);
        if (!is_peak) {
            continue;
        }

        const int64_t t_us = m->ts_us[idx1];
        if (last_peak_us != INT64_MIN) {
            const int64_t dt_us = t_us - last_peak_us;
            if (dt_us < min_period_us) {
                continue;
            }
            if (max_period_us > 0 && dt_us > max_period_us) {
                last_peak_us = t_us;
                continue;
            }
            sum_period_us += dt_us;
            periods++;
        }
        last_peak_us = t_us;
    }

    if (periods <= 0 || sum_period_us <= 0) {
        return 0.0f;
    }

    const float avg_period_s = (float) ((double) sum_period_us / (double) periods / 1000000.0);
    if (avg_period_s <= 1e-6f) {
        return 0.0f;
    }
    return 1.0f / avg_period_s;
}

static void compute_window_metrics(const pilot_motion_handle_t m,
                                   float *out_shake_rms_g,
                                   float *out_shake_peak_g,
                                   float *out_shake_freq_hz,
                                   float *out_rot_rms_dps,
                                   float *out_rot_peak_dps,
                                   float *out_rot_freq_hz)
{
    if (!m || m->count == 0) {
        if (out_shake_rms_g) *out_shake_rms_g = 0.0f;
        if (out_shake_peak_g) *out_shake_peak_g = 0.0f;
        if (out_shake_freq_hz) *out_shake_freq_hz = 0.0f;
        if (out_rot_rms_dps) *out_rot_rms_dps = 0.0f;
        if (out_rot_peak_dps) *out_rot_peak_dps = 0.0f;
        if (out_rot_freq_hz) *out_rot_freq_hz = 0.0f;
        return;
    }

    float sum_lin_sq = 0.0f;
    float max_lin_sq = 0.0f;
    float sum_gyro_sq = 0.0f;
    float max_gyro_sq = 0.0f;

    float sum_lin[3] = {0};
    float sum_lin_sq_axis[3] = {0};
    float sum_gyro[3] = {0};
    float sum_gyro_sq_axis[3] = {0};

    for (size_t i = 0; i < m->count; i++) {
        const size_t idx = ring_index_at(m, i);

        const float lx = m->lin_x[idx];
        const float ly = m->lin_y[idx];
        const float lz = m->lin_z[idx];

        const float wx = m->gyro_x[idx];
        const float wy = m->gyro_y[idx];
        const float wz = m->gyro_z[idx];

        const float lin_sq = lx * lx + ly * ly + lz * lz;
        sum_lin_sq += lin_sq;
        if (lin_sq > max_lin_sq) max_lin_sq = lin_sq;

        const float gyro_sq = wx * wx + wy * wy + wz * wz;
        sum_gyro_sq += gyro_sq;
        if (gyro_sq > max_gyro_sq) max_gyro_sq = gyro_sq;

        sum_lin[0] += lx;
        sum_lin[1] += ly;
        sum_lin[2] += lz;
        sum_lin_sq_axis[0] += lx * lx;
        sum_lin_sq_axis[1] += ly * ly;
        sum_lin_sq_axis[2] += lz * lz;

        sum_gyro[0] += wx;
        sum_gyro[1] += wy;
        sum_gyro[2] += wz;
        sum_gyro_sq_axis[0] += wx * wx;
        sum_gyro_sq_axis[1] += wy * wy;
        sum_gyro_sq_axis[2] += wz * wz;
    }

    const float n_inv = 1.0f / (float) m->count;

    const float shake_rms = safe_sqrtf(sum_lin_sq * n_inv);
    const float shake_peak = safe_sqrtf(max_lin_sq);
    const float rot_rms = safe_sqrtf(sum_gyro_sq * n_inv);
    const float rot_peak = safe_sqrtf(max_gyro_sq);

    if (out_shake_rms_g) *out_shake_rms_g = shake_rms;
    if (out_shake_peak_g) *out_shake_peak_g = shake_peak;
    if (out_rot_rms_dps) *out_rot_rms_dps = rot_rms;
    if (out_rot_peak_dps) *out_rot_peak_dps = rot_peak;

    // Choose dominant axis (max variance) for frequency estimation.
    float mean_lin[3] = {sum_lin[0] * n_inv, sum_lin[1] * n_inv, sum_lin[2] * n_inv};
    float var_lin[3] = {
        fmaxf(0.0f, sum_lin_sq_axis[0] * n_inv - mean_lin[0] * mean_lin[0]),
        fmaxf(0.0f, sum_lin_sq_axis[1] * n_inv - mean_lin[1] * mean_lin[1]),
        fmaxf(0.0f, sum_lin_sq_axis[2] * n_inv - mean_lin[2] * mean_lin[2]),
    };
    int lin_axis = 0;
    if (var_lin[1] > var_lin[lin_axis]) lin_axis = 1;
    if (var_lin[2] > var_lin[lin_axis]) lin_axis = 2;

    float mean_gyro[3] = {sum_gyro[0] * n_inv, sum_gyro[1] * n_inv, sum_gyro[2] * n_inv};
    float var_gyro[3] = {
        fmaxf(0.0f, sum_gyro_sq_axis[0] * n_inv - mean_gyro[0] * mean_gyro[0]),
        fmaxf(0.0f, sum_gyro_sq_axis[1] * n_inv - mean_gyro[1] * mean_gyro[1]),
        fmaxf(0.0f, sum_gyro_sq_axis[2] * n_inv - mean_gyro[2] * mean_gyro[2]),
    };
    int gyro_axis = 0;
    if (var_gyro[1] > var_gyro[gyro_axis]) gyro_axis = 1;
    if (var_gyro[2] > var_gyro[gyro_axis]) gyro_axis = 2;

    const float std_lin = safe_sqrtf(var_lin[lin_axis]);
    const float std_gyro = safe_sqrtf(var_gyro[gyro_axis]);

    const float sigma = (m->cfg.peak_threshold_sigma > 0.0f) ? m->cfg.peak_threshold_sigma : 0.0f;
    const float thr_lin = fmaxf(m->cfg.shake_peak_abs_threshold_g, sigma * std_lin);
    const float thr_gyro = fmaxf(m->cfg.rot_peak_abs_threshold_dps, sigma * std_gyro);

    const int64_t min_period_us = (m->cfg.max_freq_hz > 0.0f) ? (int64_t) (US_PER_S / (double) m->cfg.max_freq_hz) : 0;
    const int64_t max_period_us = (m->cfg.min_freq_hz > 0.0f) ? (int64_t) (US_PER_S / (double) m->cfg.min_freq_hz) : 0;

    const float *lin_axis_arr = (lin_axis == 0) ? m->lin_x : (lin_axis == 1) ? m->lin_y : m->lin_z;
    const float *gyro_axis_arr = (gyro_axis == 0) ? m->gyro_x : (gyro_axis == 1) ? m->gyro_y : m->gyro_z;

    float shake_f = estimate_freq_peaks_axis(m, lin_axis_arr, mean_lin[lin_axis], thr_lin, min_period_us, max_period_us);
    float rot_f = estimate_freq_peaks_axis(m, gyro_axis_arr, mean_gyro[gyro_axis], thr_gyro, min_period_us, max_period_us);

    if (m->cfg.min_freq_hz > 0.0f) {
        if (shake_f < m->cfg.min_freq_hz) shake_f = 0.0f;
        if (rot_f < m->cfg.min_freq_hz) rot_f = 0.0f;
    }
    if (m->cfg.max_freq_hz > 0.0f) {
        if (shake_f > m->cfg.max_freq_hz) shake_f = 0.0f;
        if (rot_f > m->cfg.max_freq_hz) rot_f = 0.0f;
    }

    if (out_shake_freq_hz) *out_shake_freq_hz = shake_f;
    if (out_rot_freq_hz) *out_rot_freq_hz = rot_f;
}

esp_err_t pilot_motion_create(const pilot_motion_config_t *cfg, pilot_motion_handle_t *out_handle)
{
    if (!cfg || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->sample_rate_hz <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    pilot_motion_handle_t m = calloc(1, sizeof(*m));
    if (!m) {
        return ESP_ERR_NO_MEM;
    }
    m->cfg = *cfg;

    if (m->cfg.window_s <= 0.0f) m->cfg.window_s = 2.0f;
    if (m->cfg.peak_threshold_sigma < 0.0f) m->cfg.peak_threshold_sigma = 0.0f;
    if (m->cfg.gravity_g <= 0.0f) m->cfg.gravity_g = 1.0f;

    const float cap_f = clampf(m->cfg.sample_rate_hz * m->cfg.window_s, 8.0f, 4096.0f);
    m->cap = (size_t) ceilf(cap_f);
    m->count = 0;
    m->head = 0;
    m->last_ts_us = 0;

    m->ts_us = calloc(m->cap, sizeof(*m->ts_us));
    m->lin_x = calloc(m->cap, sizeof(*m->lin_x));
    m->lin_y = calloc(m->cap, sizeof(*m->lin_y));
    m->lin_z = calloc(m->cap, sizeof(*m->lin_z));
    m->gyro_x = calloc(m->cap, sizeof(*m->gyro_x));
    m->gyro_y = calloc(m->cap, sizeof(*m->gyro_y));
    m->gyro_z = calloc(m->cap, sizeof(*m->gyro_z));

    if (!m->ts_us || !m->lin_x || !m->lin_y || !m->lin_z || !m->gyro_x || !m->gyro_y || !m->gyro_z) {
        pilot_motion_destroy(m);
        return ESP_ERR_NO_MEM;
    }

    *out_handle = m;
    return ESP_OK;
}

void pilot_motion_destroy(pilot_motion_handle_t motion)
{
    if (!motion) {
        return;
    }
    free(motion->ts_us);
    free(motion->lin_x);
    free(motion->lin_y);
    free(motion->lin_z);
    free(motion->gyro_x);
    free(motion->gyro_y);
    free(motion->gyro_z);
    free(motion);
}

void pilot_motion_reset(pilot_motion_handle_t motion)
{
    if (!motion) {
        return;
    }
    motion->count = 0;
    motion->head = 0;
    motion->last_ts_us = 0;
    if (motion->ts_us) memset(motion->ts_us, 0, motion->cap * sizeof(*motion->ts_us));
    if (motion->lin_x) memset(motion->lin_x, 0, motion->cap * sizeof(*motion->lin_x));
    if (motion->lin_y) memset(motion->lin_y, 0, motion->cap * sizeof(*motion->lin_y));
    if (motion->lin_z) memset(motion->lin_z, 0, motion->cap * sizeof(*motion->lin_z));
    if (motion->gyro_x) memset(motion->gyro_x, 0, motion->cap * sizeof(*motion->gyro_x));
    if (motion->gyro_y) memset(motion->gyro_y, 0, motion->cap * sizeof(*motion->gyro_y));
    if (motion->gyro_z) memset(motion->gyro_z, 0, motion->cap * sizeof(*motion->gyro_z));
}

esp_err_t pilot_motion_update(pilot_motion_handle_t motion,
                              const pilot_imu_reading_t *imu,
                              pilot_motion_reading_t *out_motion)
{
    if (!motion || !imu || !out_motion) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t ts_us = imu->timestamp_us;
    if (ts_us <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Reset window if time goes backwards (e.g. after esp_restart).
    if (motion->last_ts_us != 0 && ts_us <= motion->last_ts_us) {
        pilot_motion_reset(motion);
    }
    motion->last_ts_us = ts_us;

    const pilot_imu_axis3f_t a_body = imu->accel_g;
    const pilot_imu_axis3f_t w_body = imu->gyro_dps;

    const float qw = imu->quat_w;
    const float qx = imu->quat_x;
    const float qy = imu->quat_y;
    const float qz = imu->quat_z;

    const pilot_imu_axis3f_t a_nav = quat_rotate_body_to_nav(qw, qx, qy, qz, a_body);
    const pilot_imu_axis3f_t w_nav = quat_rotate_body_to_nav(qw, qx, qy, qz, w_body);

    const float g = (motion->cfg.gravity_g > 0.0f) ? motion->cfg.gravity_g : 1.0f;
    const pilot_imu_axis3f_t lin_nav = {a_nav.x, a_nav.y, a_nav.z - g};

    // Push into ring buffer.
    const size_t idx = motion->head;
    motion->ts_us[idx] = ts_us;
    motion->lin_x[idx] = lin_nav.x;
    motion->lin_y[idx] = lin_nav.y;
    motion->lin_z[idx] = lin_nav.z;
    motion->gyro_x[idx] = w_nav.x;
    motion->gyro_y[idx] = w_nav.y;
    motion->gyro_z[idx] = w_nav.z;

    motion->head = (motion->head + 1) % motion->cap;
    if (motion->count < motion->cap) {
        motion->count++;
    }

    const float lin_mag = safe_sqrtf(lin_nav.x * lin_nav.x + lin_nav.y * lin_nav.y + lin_nav.z * lin_nav.z);
    const float gyro_mag = safe_sqrtf(w_nav.x * w_nav.x + w_nav.y * w_nav.y + w_nav.z * w_nav.z);

    float shake_rms_g = 0.0f;
    float shake_peak_g = 0.0f;
    float shake_freq_hz = 0.0f;
    float rot_rms_dps = 0.0f;
    float rot_peak_dps = 0.0f;
    float rot_freq_hz = 0.0f;
    compute_window_metrics(motion,
                           &shake_rms_g,
                           &shake_peak_g,
                           &shake_freq_hz,
                           &rot_rms_dps,
                           &rot_peak_dps,
                           &rot_freq_hz);

    *out_motion = (pilot_motion_reading_t) {
        .accel_nav_g = a_nav,
        .lin_accel_nav_g = lin_nav,
        .gyro_nav_dps = w_nav,
        .lin_accel_mag_g = lin_mag,
        .gyro_mag_dps = gyro_mag,
        .shake_rms_g = shake_rms_g,
        .shake_peak_g = shake_peak_g,
        .shake_freq_hz = shake_freq_hz,
        .rot_rms_dps = rot_rms_dps,
        .rot_peak_dps = rot_peak_dps,
        .rot_freq_hz = rot_freq_hz,
    };

    return ESP_OK;
}

