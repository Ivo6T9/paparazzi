/*
 * Copyright (C) 2008-2013 The Paparazzi Team
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * @file subsystems/ahrs/ahrs_int_cmpl_quat.c
 *
 * Quaternion complementary filter (fixed-point).
 *
 * Estimate the attitude, heading and gyro bias.
 *
 */

#include "generated/airframe.h"

#include "subsystems/ahrs/ahrs_int_cmpl_quat.h"
#include "subsystems/ahrs/ahrs_aligner.h"
#include "subsystems/ahrs/ahrs_int_utils.h"

#include "state.h"

#include "subsystems/imu.h"
#if USE_GPS
#include "subsystems/gps.h"
#endif
#include "math/pprz_trig_int.h"
#include "math/pprz_algebra_int.h"

#ifdef AHRS_PROPAGATE_LOW_PASS_RATES
PRINT_CONFIG_MSG("LOW PASS FILTER ON GYRO RATES")
#endif

#ifdef AHRS_MAG_UPDATE_YAW_ONLY
#error "The define AHRS_MAG_UPDATE_YAW_ONLY doesn't exist anymore, please remove it. This is the default behaviour. Define AHRS_MAG_UPDATE_ALL_AXES to use mag for all axes and not only yaw."
#endif

#if USE_MAGNETOMETER && AHRS_USE_GPS_HEADING
#warning "Using both magnetometer and GPS course to update heading. Probably better to configure USE_MAGNETOMETER=0 if you want to use GPS course."
#endif

#if !USE_MAGNETOMETER && !AHRS_USE_GPS_HEADING
#warning "Please use either USE_MAGNETOMETER or AHRS_USE_GPS_HEADING."
#endif

#if AHRS_USE_GPS_HEADING && !USE_GPS
#error "AHRS_USE_GPS_HEADING needs USE_GPS to be TRUE"
#endif


/*
 * default gains for correcting attitude and bias from accel/mag
 */
#ifndef AHRS_ACCEL_OMEGA
#define AHRS_ACCEL_OMEGA 0.063
#endif
#ifndef AHRS_ACCEL_ZETA
#define AHRS_ACCEL_ZETA 0.9
#endif
PRINT_CONFIG_VAR(AHRS_ACCEL_OMEGA)
PRINT_CONFIG_VAR(AHRS_ACCEL_ZETA)


#ifndef AHRS_MAG_OMEGA
#define AHRS_MAG_OMEGA 0.04
#endif
#ifndef AHRS_MAG_ZETA
#define AHRS_MAG_ZETA 0.9
#endif
#if USE_MAGNETOMETER
PRINT_CONFIG_VAR(AHRS_MAG_OMEGA)
PRINT_CONFIG_VAR(AHRS_MAG_ZETA)
#endif

/** by default use the gravity heuristic to reduce gain */
#ifndef AHRS_GRAVITY_HEURISTIC_FACTOR
#define AHRS_GRAVITY_HEURISTIC_FACTOR 30
#endif

/** don't update gyro bias if heading deviation is above this threshold in degrees */
#ifndef AHRS_BIAS_UPDATE_HEADING_THRESHOLD
#define AHRS_BIAS_UPDATE_HEADING_THRESHOLD 5.0
#endif

/** Minimum speed in m/s for heading update via GPS.
 * Don't update heading from GPS course if GPS ground speed is below is this threshold
 */
#ifndef AHRS_HEADING_UPDATE_GPS_MIN_SPEED
#define AHRS_HEADING_UPDATE_GPS_MIN_SPEED 5.0
#endif

struct AhrsIntCmplQuat ahrs_impl;

static inline void set_body_state_from_quat(void);
static inline void UNUSED ahrs_update_mag_full(float dt);
static inline void ahrs_update_mag_2d(float dt);

#if PERIODIC_TELEMETRY
#include "subsystems/datalink/telemetry.h"

static void send_quat(struct transport_tx *trans, struct link_device *dev) {
  struct Int32Quat* quat = stateGetNedToBodyQuat_i();
  pprz_msg_send_AHRS_QUAT_INT(trans, dev, AC_ID,
      &ahrs_impl.weight,
      &ahrs_impl.ltp_to_imu_quat.qi,
      &ahrs_impl.ltp_to_imu_quat.qx,
      &ahrs_impl.ltp_to_imu_quat.qy,
      &ahrs_impl.ltp_to_imu_quat.qz,
      &(quat->qi),
      &(quat->qx),
      &(quat->qy),
      &(quat->qz));
}

static void send_euler(struct transport_tx *trans, struct link_device *dev) {
  struct Int32Eulers ltp_to_imu_euler;
  int32_eulers_of_quat(&ltp_to_imu_euler, &ahrs_impl.ltp_to_imu_quat);
  struct Int32Eulers* eulers = stateGetNedToBodyEulers_i();
  pprz_msg_send_AHRS_EULER_INT(trans, dev, AC_ID,
      &ltp_to_imu_euler.phi,
      &ltp_to_imu_euler.theta,
      &ltp_to_imu_euler.psi,
      &(eulers->phi),
      &(eulers->theta),
      &(eulers->psi));
}

static void send_bias(struct transport_tx *trans, struct link_device *dev) {
  pprz_msg_send_AHRS_GYRO_BIAS_INT(trans, dev, AC_ID,
      &ahrs_impl.gyro_bias.p, &ahrs_impl.gyro_bias.q, &ahrs_impl.gyro_bias.r);
}

static void send_geo_mag(struct transport_tx *trans, struct link_device *dev) {
  struct FloatVect3 h_float;
  h_float.x = MAG_FLOAT_OF_BFP(ahrs_impl.mag_h.x);
  h_float.y = MAG_FLOAT_OF_BFP(ahrs_impl.mag_h.y);
  h_float.z = MAG_FLOAT_OF_BFP(ahrs_impl.mag_h.z);
  pprz_msg_send_GEO_MAG(trans, dev, AC_ID,
                        &h_float.x, &h_float.y, &h_float.z);
}
#endif

void ahrs_init(void) {

  ahrs.status = AHRS_UNINIT;
  ahrs_impl.ltp_vel_norm_valid = FALSE;
  ahrs_impl.heading_aligned = FALSE;

  /* set ltp_to_imu so that body is zero */
  memcpy(&ahrs_impl.ltp_to_imu_quat, orientationGetQuat_i(&imu.body_to_imu),
         sizeof(struct Int32Quat));
  INT_RATES_ZERO(ahrs_impl.imu_rate);

  INT_RATES_ZERO(ahrs_impl.gyro_bias);
  INT_RATES_ZERO(ahrs_impl.rate_correction);
  INT_RATES_ZERO(ahrs_impl.high_rez_bias);

  /* set default filter cut-off frequency and damping */
  ahrs_impl.accel_omega = AHRS_ACCEL_OMEGA;
  ahrs_impl.accel_zeta = AHRS_ACCEL_ZETA;
  ahrs_set_accel_gains();
  ahrs_impl.mag_omega = AHRS_MAG_OMEGA;
  ahrs_impl.mag_zeta = AHRS_MAG_ZETA;
  ahrs_set_mag_gains();

  /* set default gravity heuristic */
  ahrs_impl.gravity_heuristic_factor = AHRS_GRAVITY_HEURISTIC_FACTOR;

#if AHRS_GRAVITY_UPDATE_COORDINATED_TURN
  ahrs_impl.correct_gravity = TRUE;
#else
  ahrs_impl.correct_gravity = FALSE;
#endif

  VECT3_ASSIGN(ahrs_impl.mag_h, MAG_BFP_OF_REAL(AHRS_H_X),
               MAG_BFP_OF_REAL(AHRS_H_Y), MAG_BFP_OF_REAL(AHRS_H_Z));

  ahrs_impl.accel_cnt = 0;
  ahrs_impl.mag_cnt = 0;

#if PERIODIC_TELEMETRY
  register_periodic_telemetry(DefaultPeriodic, "AHRS_QUAT_INT", send_quat);
  register_periodic_telemetry(DefaultPeriodic, "AHRS_EULER_INT", send_euler);
  register_periodic_telemetry(DefaultPeriodic, "AHRS_GYRO_BIAS_INT", send_bias);
  register_periodic_telemetry(DefaultPeriodic, "GEO_MAG", send_geo_mag);
#endif

}


void ahrs_align(void) {

#if USE_MAGNETOMETER
  /* Compute an initial orientation from accel and mag directly as quaternion */
  ahrs_int_get_quat_from_accel_mag(&ahrs_impl.ltp_to_imu_quat,
                                   &ahrs_aligner.lp_accel, &ahrs_aligner.lp_mag);
  ahrs_impl.heading_aligned = TRUE;
#else
  /* Compute an initial orientation from accel and just set heading to zero */
  ahrs_int_get_quat_from_accel(&ahrs_impl.ltp_to_imu_quat, &ahrs_aligner.lp_accel);
  ahrs_impl.heading_aligned = FALSE;
#endif

  set_body_state_from_quat();

  /* Use low passed gyro value as initial bias */
  RATES_COPY(ahrs_impl.gyro_bias, ahrs_aligner.lp_gyro);
  RATES_COPY(ahrs_impl.high_rez_bias, ahrs_aligner.lp_gyro);
  INT_RATES_LSHIFT(ahrs_impl.high_rez_bias, ahrs_impl.high_rez_bias, 28);

  ahrs.status = AHRS_RUNNING;
}


void ahrs_propagate(float dt) {
  int32_t freq = (int32_t)(1./dt);

  /* unbias gyro             */
  struct Int32Rates omega;
  RATES_DIFF(omega, imu.gyro_prev, ahrs_impl.gyro_bias);

  /* low pass rate */
#ifdef AHRS_PROPAGATE_LOW_PASS_RATES
  RATES_SMUL(ahrs_impl.imu_rate, ahrs_impl.imu_rate,2);
  RATES_ADD(ahrs_impl.imu_rate, omega);
  RATES_SDIV(ahrs_impl.imu_rate, ahrs_impl.imu_rate, 3);
#else
  RATES_COPY(ahrs_impl.imu_rate, omega);
#endif

  /* add correction */
  RATES_ADD(omega, ahrs_impl.rate_correction);
  /* and zeros it */
  INT_RATES_ZERO(ahrs_impl.rate_correction);

  /* integrate quaternion */
  int32_quat_integrate_fi(&ahrs_impl.ltp_to_imu_quat, &ahrs_impl.high_rez_quat,
                          &omega, freq);
  int32_quat_normalize(&ahrs_impl.ltp_to_imu_quat);

  set_body_state_from_quat();

  // increase accel and mag propagation counters
  ahrs_impl.accel_cnt++;
  ahrs_impl.mag_cnt++;
}


void ahrs_set_accel_gains(void) {
  /* Complementary filter proportionnal gain (without frequency correction)
   * Kp = 2 * omega * zeta
   *
   * accel_inv_kp = 1 / (Kp * FRAC_conversion / cross_product_gain)
   * accel_inv_kp = 4096 * 9.81 / Kp
   */
  ahrs_impl.accel_inv_kp = 4096 * 9.81 /
    (2 * ahrs_impl.accel_omega * ahrs_impl.accel_zeta);

  /* Complementary filter integral gain
   * Ki = omega^2
   *
   * accel_inv_ki = 2^5 / (Ki * FRAC_conversion / cross_product_gain)
   * accel_inv_ki = 2^5 / 2^16 * 9.81 / Ki
   * accel_inv_ki = 9.81 / 2048 / Ki
   */
  ahrs_impl.accel_inv_ki = 9.81 / 2048 / (ahrs_impl.accel_omega * ahrs_impl.accel_omega);
}

void ahrs_update_accel(float dt) {
  // check if we had at least one propagation since last update
  if (ahrs_impl.accel_cnt == 0)
    return;

  // c2 = ltp z-axis in imu-frame
  struct Int32RMat ltp_to_imu_rmat;
  int32_rmat_of_quat(&ltp_to_imu_rmat, &ahrs_impl.ltp_to_imu_quat);
  struct Int32Vect3 c2 = { RMAT_ELMT(ltp_to_imu_rmat, 0,2),
                           RMAT_ELMT(ltp_to_imu_rmat, 1,2),
                           RMAT_ELMT(ltp_to_imu_rmat, 2,2)};
  struct Int32Vect3 residual;

  struct Int32Vect3 pseudo_gravity_measurement;

  if (ahrs_impl.correct_gravity && ahrs_impl.ltp_vel_norm_valid) {
    /*
     * centrifugal acceleration in body frame
     * a_c_body = omega x (omega x r)
     * (omega x r) = tangential velocity in body frame
     * a_c_body = omega x vel_tangential_body
     * assumption: tangential velocity only along body x-axis
     */

    // FIXME: check overflows !
#define COMPUTATION_FRAC 16
#define ACC_FROM_CROSS_FRAC INT32_RATE_FRAC + INT32_SPEED_FRAC - INT32_ACCEL_FRAC - COMPUTATION_FRAC

    const struct Int32Vect3 vel_tangential_body =
      {ahrs_impl.ltp_vel_norm >> COMPUTATION_FRAC, 0, 0};
    struct Int32Vect3 acc_c_body;
    VECT3_RATES_CROSS_VECT3(acc_c_body, (*stateGetBodyRates_i()), vel_tangential_body);
    INT32_VECT3_RSHIFT(acc_c_body, acc_c_body, ACC_FROM_CROSS_FRAC);

    /* convert centrifucal acceleration from body to imu frame */
    struct Int32Vect3 acc_c_imu;
    struct Int32RMat *body_to_imu_rmat = orientationGetRMat_i(&imu.body_to_imu);
    int32_rmat_vmult(&acc_c_imu, body_to_imu_rmat, &acc_c_body);

    /* and subtract it from imu measurement to get a corrected measurement
     * of the gravity vector */
    VECT3_DIFF(pseudo_gravity_measurement, imu.accel, acc_c_imu);
  } else {
    VECT3_COPY(pseudo_gravity_measurement, imu.accel);
  }

  /* compute the residual of the pseudo gravity vector in imu frame */
  VECT3_CROSS_PRODUCT(residual, pseudo_gravity_measurement, c2);


  /* FIR filtered pseudo_gravity_measurement */
  #define FIR_FILTER_SIZE 8
  static struct Int32Vect3 filtered_gravity_measurement = {0, 0, 0};
  VECT3_SMUL(filtered_gravity_measurement, filtered_gravity_measurement, FIR_FILTER_SIZE-1);
  VECT3_ADD(filtered_gravity_measurement, pseudo_gravity_measurement);
  VECT3_SDIV(filtered_gravity_measurement, filtered_gravity_measurement, FIR_FILTER_SIZE);


  if (ahrs_impl.gravity_heuristic_factor) {
    /* heuristic on acceleration (gravity estimate) norm */
    /* Factor how strongly to change the weight.
     * e.g. for gravity_heuristic_factor 30:
     * <0.66G = 0, 1G = 1.0, >1.33G = 0
     */

    struct FloatVect3 g_meas_f;
    ACCELS_FLOAT_OF_BFP(g_meas_f, filtered_gravity_measurement);
    const float g_meas_norm = FLOAT_VECT3_NORM(g_meas_f)/9.81;
    ahrs_impl.weight = 1.0 - ahrs_impl.gravity_heuristic_factor * fabs(1.0 - g_meas_norm) / 10;
    Bound(ahrs_impl.weight, 0.15, 1.0);
  }
  else {
    ahrs_impl.weight = 1.0;
  }

  /* Complementary filter proportional gain.
   * Kp = 2 * zeta * omega
   * final Kp with frequency correction = Kp * ahrs_impl.accel_cnt
   * with ahrs_impl.accel_cnt beeing the number of propagations since last update
   *
   * residual FRAC : ACCEL_FRAC + TRIG_FRAC = 10 + 14 = 24
   * rate_correction FRAC: RATE_FRAC = 12
   * FRAC_conversion: 2^12 / 2^24 = 1 / 4096
   * cross_product_gain : 9.81 m/s2
   *
   * accel_inv_kp = 1 / (Kp * FRAC_conversion / cross_product_gain)
   * accel_inv_kp = 4096 * 9.81 / Kp
   *
   * inv_rate_scale = 1 / (weight * Kp * FRAC_conversion / cross_product_gain)
   * inv_rate_scale = 1 / Kp / weight
   * inv_rate_scale = accel_inv_kp / accel_cnt / weight
   */
  int32_t inv_rate_scale = (int32_t)(ahrs_impl.accel_inv_kp / ahrs_impl.accel_cnt
                                     / ahrs_impl.weight);
  Bound(inv_rate_scale, 8192, 4194304);

  ahrs_impl.rate_correction.p -= residual.x / inv_rate_scale;
  ahrs_impl.rate_correction.q -= residual.y / inv_rate_scale;
  ahrs_impl.rate_correction.r -= residual.z / inv_rate_scale;

  // reset accel propagation counter
  ahrs_impl.accel_cnt = 0;

  /* Complementary filter integral gain
   * Correct the gyro bias.
   * Ki = omega^2 * dt
   *
   * residual FRAC = ACCEL_FRAC + TRIG_FRAC = 10 + 14 = 24
   * high_rez_bias = RATE_FRAC+28 = 40
   * FRAC_conversion: 2^40 / 2^24 = 2^16
   * cross_product_gain : 9.81 m/s2
   *
   * accel_inv_ki = 2^5 / (Ki * FRAC_conversion / cross_product_gain)
   * accel_inv_ki = 2^5 / 2^16 * 9.81 * Ki = 9.81 / 2^11 * Ki
   *
   * inv_bias_gain = 2^5 / (weight^2 * Ki * FRAC_conversion / cross_product_gain)
   * inv_bias_gain = accel_inv_ki / weight^2
   */

  int32_t inv_bias_gain = (int32_t)(ahrs_impl.accel_inv_ki /
                                    (dt * ahrs_impl.weight * ahrs_impl.weight));
  Bound(inv_bias_gain, 8, 65536)
  ahrs_impl.high_rez_bias.p += (residual.x / inv_bias_gain) << 5;
  ahrs_impl.high_rez_bias.q += (residual.y / inv_bias_gain) << 5;
  ahrs_impl.high_rez_bias.r += (residual.z / inv_bias_gain) << 5;

  INT_RATES_RSHIFT(ahrs_impl.gyro_bias, ahrs_impl.high_rez_bias, 28);

}


void ahrs_update_mag(float dt) {
  // check if we had at least one propagation since last update
  if (ahrs_impl.mag_cnt == 0)
    return;
#if AHRS_MAG_UPDATE_ALL_AXES
  ahrs_update_mag_full(dt);
#else
  ahrs_update_mag_2d(dt);
#endif
  // reset mag propagation counter
  ahrs_impl.mag_cnt = 0;
}

void ahrs_set_mag_gains(void) {
  /* Complementary filter proportionnal gain = 2*omega*zeta */
  ahrs_impl.mag_kp = 2 * ahrs_impl.mag_zeta * ahrs_impl.mag_omega;
  /* Complementary filter integral gain = omega^2 */
  ahrs_impl.mag_ki = ahrs_impl.mag_omega * ahrs_impl.mag_omega;
}


static inline void ahrs_update_mag_full(float dt) {

  struct Int32RMat ltp_to_imu_rmat;
  int32_rmat_of_quat(&ltp_to_imu_rmat, &ahrs_impl.ltp_to_imu_quat);

  struct Int32Vect3 expected_imu;
  int32_rmat_vmult(&expected_imu, &ltp_to_imu_rmat, &ahrs_impl.mag_h);

  struct Int32Vect3 residual;
  VECT3_CROSS_PRODUCT(residual, imu.mag, expected_imu);

  /* Complementary filter proportionnal gain.
   * Kp = 2 * mag_zeta * mag_omega
   * final Kp with frequency correction = Kp * ahrs_impl.mag_cnt
   * with ahrs_impl.mag_cnt beeing the number of propagations since last update
   *
   * residual FRAC: 2 * MAG_FRAC = 22
   * rate_correction FRAC: RATE_FRAC = 12
   * FRAC conversion: 2^12 / 2^22 = 1/1024
   *
   * inv_rate_gain = 1 / Kp / FRAC_conversion
   * inv_rate_gain = 1024 / Kp
   */

  const int32_t inv_rate_gain = (int32_t)(1024.0 / (ahrs_impl.mag_kp * ahrs_impl.mag_cnt));

  ahrs_impl.rate_correction.p += residual.x / inv_rate_gain;
  ahrs_impl.rate_correction.q += residual.y / inv_rate_gain;
  ahrs_impl.rate_correction.r += residual.z / inv_rate_gain;

  /* Complementary filter integral gain
   * Correct the gyro bias.
   * Ki = omega^2 * dt
   *
   * residual FRAC: 2* MAG_FRAC = 22
   * high_rez_bias FRAC: RATE_FRAC+28 = 40
   * FRAC conversion: 2^40 / 2^22 = 2^18
   *
   * bias_gain = Ki * FRAC_conversion = Ki * 2^18
   */
  const int32_t bias_gain = (int32_t)(ahrs_impl.mag_ki * dt * (1<<18));

  ahrs_impl.high_rez_bias.p -= residual.x * bias_gain;
  ahrs_impl.high_rez_bias.q -= residual.y * bias_gain;
  ahrs_impl.high_rez_bias.r -= residual.z * bias_gain;


  INT_RATES_RSHIFT(ahrs_impl.gyro_bias, ahrs_impl.high_rez_bias, 28);

}


static inline void ahrs_update_mag_2d(float dt) {

  struct Int32Vect2 expected_ltp = {ahrs_impl.mag_h.x, ahrs_impl.mag_h.y};
  /* normalize expected ltp in 2D (x,y) */
  int32_vect2_normalize(&expected_ltp, INT32_MAG_FRAC);

  struct Int32RMat ltp_to_imu_rmat;
  int32_rmat_of_quat(&ltp_to_imu_rmat, &ahrs_impl.ltp_to_imu_quat);

  struct Int32Vect3 measured_ltp;
  int32_rmat_transp_vmult(&measured_ltp, &ltp_to_imu_rmat, &imu.mag);

  /* normalize measured ltp in 2D (x,y) */
  struct Int32Vect2 measured_ltp_2d = {measured_ltp.x, measured_ltp.y};
  int32_vect2_normalize(&measured_ltp_2d, INT32_MAG_FRAC);

  /* residual_ltp FRAC: 2 * MAG_FRAC - 5 = 17 */
  struct Int32Vect3 residual_ltp =
    { 0,
      0,
      (measured_ltp_2d.x * expected_ltp.y - measured_ltp_2d.y * expected_ltp.x)/(1<<5)};


  struct Int32Vect3 residual_imu;
  int32_rmat_vmult(&residual_imu, &ltp_to_imu_rmat, &residual_ltp);

  /* Complementary filter proportionnal gain.
   * Kp = 2 * mag_zeta * mag_omega
   * final Kp with frequency correction = Kp * ahrs_impl.mag_cnt
   * with ahrs_impl.mag_cnt beeing the number of propagations since last update
   *
   * residual_imu FRAC = residual_ltp FRAC = 17
   * rate_correction FRAC: RATE_FRAC = 12
   * FRAC conversion: 2^12 / 2^17 = 1/32
   *
   * inv_rate_gain = 1 / Kp / FRAC_conversion
   * inv_rate_gain = 32 / Kp
   */
  int32_t inv_rate_gain = (int32_t)(32.0 / (ahrs_impl.mag_kp * ahrs_impl.mag_cnt));

  ahrs_impl.rate_correction.p += (residual_imu.x / inv_rate_gain);
  ahrs_impl.rate_correction.q += (residual_imu.y / inv_rate_gain);
  ahrs_impl.rate_correction.r += (residual_imu.z / inv_rate_gain);

  /* Complementary filter integral gain
   * Correct the gyro bias.
   * Ki = omega^2 * dt
   *
   * residual_imu FRAC = residual_ltp FRAC = 17
   * high_rez_bias FRAC: RATE_FRAC+28 = 40
   * FRAC conversion: 2^40 / 2^17 = 2^23
   *
   * bias_gain = Ki * FRAC_conversion = Ki * 2^23
   */
  int32_t bias_gain = (int32_t)(ahrs_impl.mag_ki * dt * (1 << 23));

  ahrs_impl.high_rez_bias.p -= (residual_imu.x * bias_gain);
  ahrs_impl.high_rez_bias.q -= (residual_imu.y * bias_gain);
  ahrs_impl.high_rez_bias.r -= (residual_imu.z * bias_gain);

  INT_RATES_RSHIFT(ahrs_impl.gyro_bias, ahrs_impl.high_rez_bias, 28);

}

void ahrs_update_gps(void) {
#if AHRS_GRAVITY_UPDATE_COORDINATED_TURN && USE_GPS
  if (gps.fix == GPS_FIX_3D) {
    ahrs_impl.ltp_vel_norm = SPEED_BFP_OF_REAL(gps.speed_3d / 100.);
    ahrs_impl.ltp_vel_norm_valid = TRUE;
  } else {
    ahrs_impl.ltp_vel_norm_valid = FALSE;
  }
#endif

#if AHRS_USE_GPS_HEADING && USE_GPS
  // got a 3d fix, ground speed > AHRS_HEADING_UPDATE_GPS_MIN_SPEED (default 5.0 m/s)
  // and course accuracy is better than 10deg
  if (gps.fix == GPS_FIX_3D &&
      gps.gspeed >= (AHRS_HEADING_UPDATE_GPS_MIN_SPEED * 100) &&
      gps.cacc <= RadOfDeg(10*1e7)) {

    // gps.course is in rad * 1e7, we need it in rad * 2^INT32_ANGLE_FRAC
    int32_t course = gps.course * ((1<<INT32_ANGLE_FRAC) / 1e7);

    /* the assumption here is that there is no side-slip, so heading=course */

    if (ahrs_impl.heading_aligned) {
      ahrs_update_heading(course);
    }
    else {
      /* hard reset the heading if this is the first measurement */
      ahrs_realign_heading(course);
    }
  }
#endif
}


void ahrs_update_heading(int32_t heading) {

  INT32_ANGLE_NORMALIZE(heading);

  // row 0 of ltp_to_body_rmat = body x-axis in ltp frame
  // we only consider x and y
  struct Int32RMat* ltp_to_body_rmat = stateGetNedToBodyRMat_i();
  struct Int32Vect2 expected_ltp =
    { RMAT_ELMT((*ltp_to_body_rmat), 0, 0),
      RMAT_ELMT((*ltp_to_body_rmat), 0, 1) };

  int32_t heading_x, heading_y;
  PPRZ_ITRIG_COS(heading_x, heading); // measured course in x-direction
  PPRZ_ITRIG_SIN(heading_y, heading); // measured course in y-direction

  // expected_heading cross measured_heading ??
  struct Int32Vect3 residual_ltp =
    { 0,
      0,
      (expected_ltp.x * heading_y - expected_ltp.y * heading_x)/(1<<INT32_ANGLE_FRAC)};

  struct Int32Vect3 residual_imu;
  struct Int32RMat ltp_to_imu_rmat;
  int32_rmat_of_quat(&ltp_to_imu_rmat, &ahrs_impl.ltp_to_imu_quat);
  int32_rmat_vmult(&residual_imu, &ltp_to_imu_rmat, &residual_ltp);

  // residual FRAC = TRIG_FRAC + TRIG_FRAC = 14 + 14 = 28
  // rate_correction FRAC = RATE_FRAC = 12
  // 2^12 / 2^28 * 4.0 = 1/2^14
  // (1<<INT32_ANGLE_FRAC)/2^14 = 1/4
  ahrs_impl.rate_correction.p += residual_imu.x/4;
  ahrs_impl.rate_correction.q += residual_imu.y/4;
  ahrs_impl.rate_correction.r += residual_imu.z/4;


  /* crude attempt to only update bias if deviation is small
   * e.g. needed when you only have gps providing heading
   * and the inital heading is totally different from
   * the gps course information you get once you have a gps fix.
   * Otherwise the bias will be falsely "corrected".
   */
  int32_t sin_max_angle_deviation;
  PPRZ_ITRIG_SIN(sin_max_angle_deviation, TRIG_BFP_OF_REAL(RadOfDeg(AHRS_BIAS_UPDATE_HEADING_THRESHOLD)));
  if (ABS(residual_ltp.z) < sin_max_angle_deviation)
  {
    // residual_ltp FRAC = 2 * TRIG_FRAC = 28
    // high_rez_bias = RATE_FRAC+28 = 40
    // 2^40 / 2^28 * 2.5e-4 = 1
    ahrs_impl.high_rez_bias.p -= residual_imu.x*(1<<INT32_ANGLE_FRAC);
    ahrs_impl.high_rez_bias.q -= residual_imu.y*(1<<INT32_ANGLE_FRAC);
    ahrs_impl.high_rez_bias.r -= residual_imu.z*(1<<INT32_ANGLE_FRAC);

    INT_RATES_RSHIFT(ahrs_impl.gyro_bias, ahrs_impl.high_rez_bias, 28);
  }
}

void ahrs_realign_heading(int32_t heading) {

  struct Int32Quat ltp_to_body_quat = *stateGetNedToBodyQuat_i();

  /* quaternion representing only the heading rotation from ltp to body */
  struct Int32Quat q_h_new;
  q_h_new.qx = 0;
  q_h_new.qy = 0;
  PPRZ_ITRIG_SIN(q_h_new.qz, heading/2);
  PPRZ_ITRIG_COS(q_h_new.qi, heading/2);

  /* quaternion representing current heading only */
  struct Int32Quat q_h;
  QUAT_COPY(q_h, ltp_to_body_quat);
  q_h.qx = 0;
  q_h.qy = 0;
  int32_quat_normalize(&q_h);

  /* quaternion representing rotation from current to new heading */
  struct Int32Quat q_c;
  int32_quat_inv_comp_norm_shortest(&q_c, &q_h, &q_h_new);

  /* correct current heading in body frame */
  struct Int32Quat q;
  int32_quat_comp_norm_shortest(&q, &q_c, &ltp_to_body_quat);
  QUAT_COPY(ltp_to_body_quat, q);

  /* compute ltp to imu rotations */
  struct Int32Quat *body_to_imu_quat = orientationGetQuat_i(&imu.body_to_imu);
  int32_quat_comp(&ahrs_impl.ltp_to_imu_quat, &ltp_to_body_quat, body_to_imu_quat);

  /* Set state */
  stateSetNedToBodyQuat_i(&ltp_to_body_quat);

  ahrs_impl.heading_aligned = TRUE;
}


/* Rotate angles and rates from imu to body frame and set state */
static inline void set_body_state_from_quat(void) {
  /* Compute LTP to BODY quaternion */
  struct Int32Quat ltp_to_body_quat;
  struct Int32Quat *body_to_imu_quat = orientationGetQuat_i(&imu.body_to_imu);
  int32_quat_comp_inv(&ltp_to_body_quat, &ahrs_impl.ltp_to_imu_quat, body_to_imu_quat);
  /* Set state */
  stateSetNedToBodyQuat_i(&ltp_to_body_quat);

  /* compute body rates */
  struct Int32Rates body_rate;
  struct Int32RMat *body_to_imu_rmat = orientationGetRMat_i(&imu.body_to_imu);
  int32_rmat_transp_ratemult(&body_rate, body_to_imu_rmat, &ahrs_impl.imu_rate);
  /* Set state */
  stateSetBodyRates_i(&body_rate);
}
