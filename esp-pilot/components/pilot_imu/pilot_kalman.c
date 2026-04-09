#include "pilot_kalman.h"

static float wrap180f(float a_deg)
{
    while (a_deg > 180.0f) a_deg -= 360.0f;
    while (a_deg < -180.0f) a_deg += 360.0f;
    return a_deg;
}

void pilot_kalman_init(pilot_kalman_t *kf, float q_angle, float q_bias, float r_measure)
{
    if (!kf) {
        return;
    }

    kf->q_angle = q_angle;
    kf->q_bias = q_bias;
    kf->r_measure = r_measure;

    kf->angle = 0.0f;
    kf->bias = 0.0f;
    kf->rate = 0.0f;

    kf->p00 = 0.0f;
    kf->p01 = 0.0f;
    kf->p10 = 0.0f;
    kf->p11 = 0.0f;
}

void pilot_kalman_set_angle(pilot_kalman_t *kf, float angle_deg)
{
    if (!kf) {
        return;
    }
    kf->angle = angle_deg;
}

float pilot_kalman_get_angle(pilot_kalman_t *kf, float new_angle_deg, float new_rate_dps, float dt_s)
{
    if (!kf) {
        return new_angle_deg;
    }
    if (dt_s <= 0.0f) {
        return kf->angle;
    }

    kf->rate = new_rate_dps - kf->bias;
    kf->angle += dt_s * kf->rate;

    kf->p00 += dt_s * (dt_s * kf->p11 - kf->p01 - kf->p10 + kf->q_angle);
    kf->p01 -= dt_s * kf->p11;
    kf->p10 -= dt_s * kf->p11;
    kf->p11 += kf->q_bias * dt_s;

    const float s = kf->p00 + kf->r_measure;
    const float k0 = kf->p00 / s;
    const float k1 = kf->p10 / s;

    const float y = wrap180f(new_angle_deg - kf->angle);
    kf->angle += k0 * y;
    kf->bias += k1 * y;

    const float p00_temp = kf->p00;
    const float p01_temp = kf->p01;

    kf->p00 -= k0 * p00_temp;
    kf->p01 -= k0 * p01_temp;
    kf->p10 -= k1 * p00_temp;
    kf->p11 -= k1 * p01_temp;

    return kf->angle;
}
