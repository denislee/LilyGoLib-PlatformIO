/**
 * @file      sensors.h
 * @brief     GPS power toggle, IMU.
 */
#pragma once

#include "types.h"

// --- GPS ---
// The GPS module is wired to power-rail control + a UART. We expose only the
// power toggle: no app currently consumes GPS data, so the parsing/PPS
// plumbing has been dropped.
bool hw_get_gps_enable();
void hw_set_gps_enable(bool en);

// --- IMU ---
void hw_register_imu_process();
void hw_unregister_imu_process();
void hw_get_imu_params(imu_params_t &params);

// True if the device is lying roughly flat with the screen pointing down
// (e.g. set face-down on a table). Always false on the emulator.
bool hw_is_face_down();

// IMU diagnostic snapshot. Lets the status-bar debug overlay (and any future
// caller) see what the IMU stack reports without having to know which sensor
// IC is on the board. All counters/booleans are 0 / false on the emulator.
struct imu_diag_t {
    bool     bhi260_online;        // BHI260AP detected at boot
    bool     bma423_online;        // BMA423 detected at boot
    uint16_t sensor_count;         // virtual sensors the firmware reports as present
    bool     grv_configured;       // configure(GAME_ROTATION_VECTOR, ...) returned true
    bool     dev_orient_configured;// configure(DEVICE_ORIENTATION, ...) returned true
    bool     accel_configured;     // configure(ACCEL_PASSTHROUGH, ...) returned true
    uint32_t grv_events;           // GAME_ROTATION_VECTOR callback fire count
    uint32_t dev_orient_events;    // DEVICE_ORIENTATION callback fire count
    uint32_t accel_events;         // ACCEL_PASSTHROUGH callback fire count
    uint8_t  dev_orient_value;     // last DEVICE_ORIENTATION byte received
    float    accel_g_x;            // last accel reading in g (≈ ±1.0 at rest)
    float    accel_g_y;
    float    accel_g_z;
};

void hw_get_imu_diag(imu_diag_t &out);
