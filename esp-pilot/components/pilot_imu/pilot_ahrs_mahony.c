#include "pilot_ahrs_mahony.h"

#include <math.h>
#include <string.h>

#define RAD_TO_DEG (57.29577951308232f)
#define DEG_TO_RAD (0.017453292519943295f)
#define PI_F (3.14159265358979323846f)

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float wrap_pi(float a)
{
    while (a > PI_F) a -= (2.0f * PI_F);
    while (a < -PI_F) a += (2.0f * PI_F);
    return a;
}

static float safe_sqrtf(float v)
{
    return sqrtf(fmaxf(v, 0.0f));
}

static pilot_ahrs_vec3f_t vec3_add(pilot_ahrs_vec3f_t a, pilot_ahrs_vec3f_t b)
{
    return (pilot_ahrs_vec3f_t) {a.x + b.x, a.y + b.y, a.z + b.z};
}

static pilot_ahrs_vec3f_t vec3_sub(pilot_ahrs_vec3f_t a, pilot_ahrs_vec3f_t b)
{
    return (pilot_ahrs_vec3f_t) {a.x - b.x, a.y - b.y, a.z - b.z};
}

static pilot_ahrs_vec3f_t vec3_scale(pilot_ahrs_vec3f_t v, float s)
{
    return (pilot_ahrs_vec3f_t) {v.x * s, v.y * s, v.z * s};
}

static float vec3_dot(pilot_ahrs_vec3f_t a, pilot_ahrs_vec3f_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static pilot_ahrs_vec3f_t vec3_cross(pilot_ahrs_vec3f_t a, pilot_ahrs_vec3f_t b)
{
    return (pilot_ahrs_vec3f_t) {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

static float vec3_norm(pilot_ahrs_vec3f_t v)
{
    return safe_sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static pilot_ahrs_vec3f_t vec3_normalize(pilot_ahrs_vec3f_t v)
{
    const float n = vec3_norm(v);
    if (n <= 1e-6f) {
        return (pilot_ahrs_vec3f_t) {0};
    }
    return vec3_scale(v, 1.0f / n);
}

static void quat_normalize(pilot_ahrs_mahony_t *f)
{
    const float n = safe_sqrtf(f->qw * f->qw + f->qx * f->qx + f->qy * f->qy + f->qz * f->qz);
    if (n <= 1e-9f) {
        f->qw = 1.0f;
        f->qx = 0.0f;
        f->qy = 0.0f;
        f->qz = 0.0f;
        return;
    }
    const float inv = 1.0f / n;
    f->qw *= inv;
    f->qx *= inv;
    f->qy *= inv;
    f->qz *= inv;
}

static void quat_from_euler_zyx(float roll_rad, float pitch_rad, float yaw_rad, float *qw, float *qx, float *qy, float *qz)
{
    const float cr = cosf(roll_rad * 0.5f);
    const float sr = sinf(roll_rad * 0.5f);
    const float cp = cosf(pitch_rad * 0.5f);
    const float sp = sinf(pitch_rad * 0.5f);
    const float cy = cosf(yaw_rad * 0.5f);
    const float sy = sinf(yaw_rad * 0.5f);

    *qw = cr * cp * cy + sr * sp * sy;
    *qx = sr * cp * cy - cr * sp * sy;
    *qy = cr * sp * cy + sr * cp * sy;
    *qz = cr * cp * sy - sr * sp * cy;
}

static void quat_to_euler_zyx(const pilot_ahrs_mahony_t *f, float *roll_rad, float *pitch_rad, float *yaw_rad)
{
    const float qw = f->qw;
    const float qx = f->qx;
    const float qy = f->qy;
    const float qz = f->qz;

    const float sinr_cosp = 2.0f * (qw * qx + qy * qz);
    const float cosr_cosp = 1.0f - 2.0f * (qx * qx + qy * qy);
    *roll_rad = atan2f(sinr_cosp, cosr_cosp);

    const float sinp = 2.0f * (qw * qy - qz * qx);
    *pitch_rad = asinf(clampf(sinp, -1.0f, 1.0f));

    const float siny_cosp = 2.0f * (qw * qz + qx * qy);
    const float cosy_cosp = 1.0f - 2.0f * (qy * qy + qz * qz);
    *yaw_rad = atan2f(siny_cosp, cosy_cosp);
}

static pilot_ahrs_vec3f_t gravity_nav_to_body(const pilot_ahrs_mahony_t *f)
{
    const float qw = f->qw;
    const float qx = f->qx;
    const float qy = f->qy;
    const float qz = f->qz;

    // g_body = q_conj * [0,0,0,1] * q (unit gravity direction), simplified.
    const float gx = 2.0f * (qx * qz - qw * qy);
    const float gy = 2.0f * (qw * qx + qy * qz);
    const float gz = qw * qw - qx * qx - qy * qy + qz * qz;
    return (pilot_ahrs_vec3f_t) {gx, gy, gz};
}

static void update_continuous_angles(pilot_ahrs_mahony_t *f)
{
    float roll_rad, pitch_rad, yaw_rad;
    quat_to_euler_zyx(f, &roll_rad, &pitch_rad, &yaw_rad);

    const float yaw_wrapped = wrap_pi(yaw_rad);
    const float dyaw = wrap_pi(yaw_wrapped - f->yaw_last_rad);
    f->yaw_last_rad = yaw_wrapped;
    f->yaw_total_rad += dyaw;

    const pilot_ahrs_vec3f_t g_b = gravity_nav_to_body(f);
    const float tr = wrap_pi(atan2f(g_b.y, g_b.z));
    const float tp = wrap_pi(atan2f(-g_b.x, g_b.z));

    const float dtr = wrap_pi(tr - f->turn_roll_last_rad);
    f->turn_roll_last_rad = tr;
    f->turn_roll_total_rad += dtr;

    const float dtp = wrap_pi(tp - f->turn_pitch_last_rad);
    f->turn_pitch_last_rad = tp;
    f->turn_pitch_total_rad += dtp;
}

static void compute_outputs(const pilot_ahrs_mahony_t *f, pilot_ahrs_vec3f_t *out_euler_deg, pilot_ahrs_vec3f_t *out_turn_deg)
{
    if (out_euler_deg) {
        float roll_rad, pitch_rad, yaw_rad;
        quat_to_euler_zyx(f, &roll_rad, &pitch_rad, &yaw_rad);
        *out_euler_deg = (pilot_ahrs_vec3f_t) {
            roll_rad * RAD_TO_DEG,
            pitch_rad * RAD_TO_DEG,
            wrap_pi(yaw_rad) * RAD_TO_DEG,
        };
    }
    if (out_turn_deg) {
        *out_turn_deg = (pilot_ahrs_vec3f_t) {
            f->turn_roll_total_rad * RAD_TO_DEG,
            f->turn_pitch_total_rad * RAD_TO_DEG,
            f->yaw_total_rad * RAD_TO_DEG,
        };
    }
}

void pilot_ahrs_mahony_init(pilot_ahrs_mahony_t *f, const pilot_ahrs_mahony_config_t *cfg)
{
    if (!f) {
        return;
    }

    memset(f, 0, sizeof(*f));
    f->qw = 1.0f;

    f->cfg.kp = (cfg && cfg->kp > 0.0f) ? cfg->kp : 2.0f;
    f->cfg.ki = (cfg && cfg->ki > 0.0f) ? cfg->ki : 0.0f;
    f->cfg.accel_gate_g = (cfg && cfg->accel_gate_g > 0.0f) ? cfg->accel_gate_g : 0.2f;
    f->cfg.accel_lpf_tau_s = (cfg && cfg->accel_lpf_tau_s > 0.0f) ? cfg->accel_lpf_tau_s : 0.02f;

    f->initialized = false;
    f->accel_lpf_initialized = false;
}

void pilot_ahrs_mahony_reset(pilot_ahrs_mahony_t *f, float roll_deg, float pitch_deg, float yaw_deg)
{
    if (!f) {
        return;
    }

    quat_from_euler_zyx(roll_deg * DEG_TO_RAD, pitch_deg * DEG_TO_RAD, yaw_deg * DEG_TO_RAD, &f->qw, &f->qx, &f->qy, &f->qz);
    quat_normalize(f);
    f->initialized = true;

    const float yaw_rad = yaw_deg * DEG_TO_RAD;
    f->yaw_last_rad = wrap_pi(yaw_rad);
    f->yaw_total_rad = yaw_rad;

    const pilot_ahrs_vec3f_t g_b = gravity_nav_to_body(f);
    const float tr = atan2f(g_b.y, g_b.z);
    const float tp = atan2f(-g_b.x, g_b.z);

    f->turn_roll_last_rad = wrap_pi(tr);
    f->turn_roll_total_rad = tr;

    f->turn_pitch_last_rad = wrap_pi(tp);
    f->turn_pitch_total_rad = tp;

    f->accel_lpf_initialized = false;
}

void pilot_ahrs_mahony_set_bias_dps(pilot_ahrs_mahony_t *f, pilot_ahrs_vec3f_t bias_dps)
{
    if (!f) {
        return;
    }
    f->bias_rad_s.x = bias_dps.x * DEG_TO_RAD;
    f->bias_rad_s.y = bias_dps.y * DEG_TO_RAD;
    f->bias_rad_s.z = bias_dps.z * DEG_TO_RAD;
}

pilot_ahrs_vec3f_t pilot_ahrs_mahony_get_bias_dps(const pilot_ahrs_mahony_t *f)
{
    if (!f) {
        return (pilot_ahrs_vec3f_t) {0};
    }
    return (pilot_ahrs_vec3f_t) {
        f->bias_rad_s.x * RAD_TO_DEG,
        f->bias_rad_s.y * RAD_TO_DEG,
        f->bias_rad_s.z * RAD_TO_DEG,
    };
}

void pilot_ahrs_mahony_update(pilot_ahrs_mahony_t *f,
                              pilot_ahrs_vec3f_t gyro_dps,
                              pilot_ahrs_vec3f_t accel_g,
                              float dt_s,
                              pilot_ahrs_vec3f_t *out_euler_deg,
                              pilot_ahrs_vec3f_t *out_turn_deg)
{
    if (!f) {
        return;
    }

    // Initialize tilt from accel on first update (yaw=0).
    if (!f->initialized) {
        const pilot_ahrs_vec3f_t a_n = vec3_normalize(accel_g);
        const float roll0 = atan2f(a_n.y, a_n.z) * RAD_TO_DEG;
        const float pitch0 = atan2f(-a_n.x, safe_sqrtf(a_n.y * a_n.y + a_n.z * a_n.z)) * RAD_TO_DEG;
        pilot_ahrs_mahony_reset(f, roll0, pitch0, 0.0f);
    }

    // Optional accel LPF.
    if (!f->accel_lpf_initialized) {
        f->accel_lpf_g = accel_g;
        f->accel_lpf_initialized = true;
    } else if (dt_s > 0.0f) {
        const float tau = f->cfg.accel_lpf_tau_s;
        if (tau > 0.0f) {
            const float alpha = clampf(dt_s / (tau + dt_s), 0.0f, 1.0f);
            f->accel_lpf_g.x = f->accel_lpf_g.x + alpha * (accel_g.x - f->accel_lpf_g.x);
            f->accel_lpf_g.y = f->accel_lpf_g.y + alpha * (accel_g.y - f->accel_lpf_g.y);
            f->accel_lpf_g.z = f->accel_lpf_g.z + alpha * (accel_g.z - f->accel_lpf_g.z);
            accel_g = f->accel_lpf_g;
        }
    }

    // Convert gyro to rad/s, subtract bias.
    pilot_ahrs_vec3f_t omega = vec3_scale(gyro_dps, DEG_TO_RAD);
    omega = vec3_sub(omega, f->bias_rad_s);

    // Accel correction (roll/pitch only; yaw not observable with only accel).
    const float amag = vec3_norm(accel_g);
    const float gate = f->cfg.accel_gate_g;
    const bool accel_ok = (amag > 1e-6f) && ((gate <= 0.0f) || (fabsf(amag - 1.0f) <= gate));
    if (accel_ok && dt_s > 0.0f) {
        const pilot_ahrs_vec3f_t a_n = vec3_scale(accel_g, 1.0f / amag);
        const pilot_ahrs_vec3f_t g_pred = vec3_normalize(gravity_nav_to_body(f));

        // Error is the rotation that aligns predicted gravity to measured gravity.
        pilot_ahrs_vec3f_t e = vec3_cross(a_n, g_pred);

        // Remove yaw component: project error onto plane perpendicular to gravity.
        const float e_par = vec3_dot(e, g_pred);
        e = vec3_sub(e, vec3_scale(g_pred, e_par));

        // Bias integral update (X/Y only).
        if (f->cfg.ki > 0.0f) {
            f->bias_rad_s.x -= f->cfg.ki * e.x * dt_s;
            f->bias_rad_s.y -= f->cfg.ki * e.y * dt_s;
        }

        // Proportional feedback on omega.
        omega.x += f->cfg.kp * e.x;
        omega.y += f->cfg.kp * e.y;
        omega.z += f->cfg.kp * e.z;
    }

    // Integrate quaternion using corrected body rates.
    if (dt_s > 0.0f) {
        const float halfdt = 0.5f * dt_s;
        const float gx = omega.x;
        const float gy = omega.y;
        const float gz = omega.z;

        const float qa = f->qw;
        const float qb = f->qx;
        const float qc = f->qy;
        const float qd = f->qz;

        f->qw = qa + (-qb * gx - qc * gy - qd * gz) * halfdt;
        f->qx = qb + (qa * gx + qc * gz - qd * gy) * halfdt;
        f->qy = qc + (qa * gy - qb * gz + qd * gx) * halfdt;
        f->qz = qd + (qa * gz + qb * gy - qc * gx) * halfdt;
        quat_normalize(f);
    }

    update_continuous_angles(f);
    compute_outputs(f, out_euler_deg, out_turn_deg);
}

