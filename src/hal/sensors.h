/**
 * @file      sensors.h
 * @brief     GPS, IMU, magnetometer, environmental sensors.
 */
#pragma once

#include "types.h"

// --- GPS ---
bool hw_get_gps_info(gps_params_t &param);
void hw_gps_attach_pps();
void hw_gps_detach_pps();
bool hw_get_gps_enable();
void hw_set_gps_enable(bool en);

// --- IMU ---
void hw_register_imu_process();
void hw_unregister_imu_process();
void hw_get_imu_params(imu_params_t &params);

// --- Magnetometer ---
void hw_mag_enable(bool enable);
float hw_mag_get_polar();

// --- BME280 (temp/humidity/pressure) ---
void hw_bme_get_data(float &temp, float &humi, float &press, float &alt);
