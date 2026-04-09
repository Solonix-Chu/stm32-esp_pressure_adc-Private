#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float q_angle;
    float q_bias;
    float r_measure;

    float angle;
    float bias;
    float rate;

    float p00;
    float p01;
    float p10;
    float p11;
} pilot_kalman_t;

void pilot_kalman_init(pilot_kalman_t *kf, float q_angle, float q_bias, float r_measure);
void pilot_kalman_set_angle(pilot_kalman_t *kf, float angle_deg);
float pilot_kalman_get_angle(pilot_kalman_t *kf, float new_angle_deg, float new_rate_dps, float dt_s);

#ifdef __cplusplus
}
#endif

