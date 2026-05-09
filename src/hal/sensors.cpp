/**
 * @file      sensors.cpp
 * @brief     GPS power toggle, IMU, magnetometer, BME280.
 */
#include "sensors.h"
#include "system.h"
#include "internal.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#else
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cstring>
#endif

// --- GPS ---------------------------------------------------------------
// Only the power-rail toggle survives. The NMEA decode + PPS interrupt path
// was removed because no app consumes GPS data; the toggle is kept so users
// can still shut down the GPS module's power rail to save battery.

bool hw_get_gps_enable() { return user_setting.gps_enable; }
void hw_set_gps_enable(bool en) {
    user_setting.gps_enable = en;
#ifdef ARDUINO
    instance.powerControl(POWER_GPS, en);
    delay(10);
    if (!en) {
        Serial1.end();
    } else {
        Serial1.begin(38400, SERIAL_8N1, GPS_RX, GPS_TX);
    }
#endif
}

// --- IMU ---------------------------------------------------------------

static imu_params_t imu_params = {0, 0, 0, 0};

void hw_get_imu_params(imu_params_t &params)
{
#ifdef ARDUINO
#if defined(USING_BHI260_SENSOR)
    if (hw_get_device_online() & HW_BHI260AP_ONLINE) {
        params =  imu_params;
    }
#elif defined(USING_BMA423_SENSOR)
    if (hw_get_device_online() & HW_BMA423_ONLINE) {
        params.orientation = instance.sensor.direction();
    }
#endif // SENSOR
#else
    params =  imu_params;
#endif //ARDUINO
}

// Diagnostic counters surfaced via hw_get_imu_diag(). Updated from the BHI260
// callbacks (which run from the LVGL task under the instance lock) and from
// hw_register_imu_process() at boot. Defined before hw_is_face_down() so the
// face-down logic can read the live accel/quaternion state.
static imu_diag_t s_imu_diag = {};

#if  defined(ARDUINO) && defined(USING_BHI260_SENSOR)
static void imu_data_process(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp, void *user_data)
{
    float roll, pitch, yaw;
    bhy2_quaternion_to_euler(data_ptr, &roll,  &pitch, &yaw);
    imu_params.roll = roll;
    imu_params.pitch = pitch;
    imu_params.heading = yaw;
    s_imu_diag.grv_events++;
}

// DEVICE_ORIENTATION delivers a single byte enum (0=portrait-up, 1=landscape-
// left, 2=portrait-down, 3=landscape-right per BHY2 reference). We surface the
// raw value so the debug overlay can show which mapping the loaded firmware
// actually uses — boards remap axes, so the spec value isn't always the same.
static void imu_dev_orient_process(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp, void *user_data)
{
    if (data_ptr && len >= 1) {
        s_imu_diag.dev_orient_value = data_ptr[0];
        imu_params.orientation = data_ptr[0];
    }
    s_imu_diag.dev_orient_events++;
}

// ACCEL_PASSTHROUGH is the lowest-level virtual sensor on the BHI260 — it's
// in every firmware variant including the GPIO-only build the LoRa Pager
// uses, which is why we lean on it as the primary face-down signal. Data is
// three int16 g-units that getScaling() converts to actual g.
static void imu_accel_process(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp, void *user_data)
{
    if (!data_ptr || len < 6) {
        s_imu_diag.accel_events++;
        return;
    }
    bhy2_data_xyz raw{};
    bhy2_parse_xyz(data_ptr, &raw);
    float scale = instance.sensor.getScaling(sensor_id);
    s_imu_diag.accel_g_x = raw.x * scale;
    s_imu_diag.accel_g_y = raw.y * scale;
    s_imu_diag.accel_g_z = raw.z * scale;
    s_imu_diag.accel_events++;
}
#endif //ARDUINO

bool hw_is_face_down()
{
#ifdef ARDUINO
#if defined(USING_BMA423_SENSOR)
    if (hw_get_device_online() & HW_BMA423_ONLINE) {
        return instance.sensor.direction() == SensorBMA423::DIRECTION_BOTTOM;
    }
#elif defined(USING_BHI260_SENSOR)
    if (hw_get_device_online() & HW_BHI260AP_ONLINE) {
        // Prefer raw accelerometer when ACCEL_PASSTHROUGH is flowing — it's
        // the lowest-level virtual sensor and works regardless of which
        // firmware variant is loaded. Face-down ≈ gravity points up out of
        // the screen, so az ≈ -1 g (sign depends on the LilyGoLib axis
        // remap applied at boot).
        if (s_imu_diag.accel_events > 0) {
            float ax = s_imu_diag.accel_g_x;
            float ay = s_imu_diag.accel_g_y;
            float az = s_imu_diag.accel_g_z;
            float mag2 = ax * ax + ay * ay + az * az;
            return mag2 > 0.7f && mag2 < 1.5f && az < -0.6f;
        }
        if (s_imu_diag.grv_events > 0) {
            float roll = imu_params.roll;
            float pitch = imu_params.pitch;
            if (roll < 0) roll = -roll;
            if (pitch < 0) pitch = -pitch;
            return roll > 150.0f && pitch < 30.0f;
        }
        if (s_imu_diag.dev_orient_events > 0) {
            return s_imu_diag.dev_orient_value == 2;
        }
    }
#endif
#endif
    return false;
}

void hw_register_imu_process()
{
#if defined(ARDUINO)
#if defined(USING_BHI260_SENSOR)
    s_imu_diag.bhi260_online = (hw_get_device_online() & HW_BHI260AP_ONLINE) != 0;
    if (s_imu_diag.bhi260_online) {
        // Dump the firmware's virtual-sensor table to Serial so the user can
        // see which IDs the loaded firmware actually exposes. Then sensor
        // count goes into the diag struct for the on-screen readout.
        BoschSensorInfo info = instance.sensor.getSensorInfo();
        info.printInfo(Serial);
        // Pinned SensorLib version lacks getAvailableSensorCount(), so walk
        // the BHY2 sensor table directly via the C helper to count what the
        // firmware actually exposes.
        uint16_t count = 0;
        if (info.dev) {
            for (uint8_t id = 1; id < BHY2_SENSOR_ID_MAX; ++id) {
                if (bhy2_is_sensor_available(id, info.dev)) count++;
            }
        }
        s_imu_diag.sensor_count = count;

        float sample_rate = 100.0f;      /* 100 Hz for accel + quaternion */
        uint32_t report_latency_ms = 0;  /* report immediately */

        // Force-configure all three regardless of what the info table says
        // — the GPIO firmware variant on the Pager has been observed to
        // misreport availability. configure() returns false harmlessly when
        // the firmware genuinely lacks the sensor, so this is safe.
        s_imu_diag.accel_configured = instance.sensor.configure(
            SensorBHI260AP::ACCEL_PASSTHROUGH, sample_rate, report_latency_ms);
        if (s_imu_diag.accel_configured) {
            instance.sensor.onResultEvent(
                SensorBHI260AP::ACCEL_PASSTHROUGH, imu_accel_process);
        }

        s_imu_diag.grv_configured = instance.sensor.configure(
            SensorBHI260AP::GAME_ROTATION_VECTOR, sample_rate,
            report_latency_ms);
        if (s_imu_diag.grv_configured) {
            instance.sensor.onResultEvent(
                SensorBHI260AP::GAME_ROTATION_VECTOR, imu_data_process);
        }

        s_imu_diag.dev_orient_configured = instance.sensor.configure(
            SensorBHI260AP::DEVICE_ORIENTATION, 5.0f, report_latency_ms);
        if (s_imu_diag.dev_orient_configured) {
            instance.sensor.onResultEvent(
                SensorBHI260AP::DEVICE_ORIENTATION, imu_dev_orient_process);
        }
    }
#elif defined(USING_BMA423_SENSOR)
    s_imu_diag.bma423_online = (hw_get_device_online() & HW_BMA423_ONLINE) != 0;
    if (s_imu_diag.bma423_online) {
        instance.sensor.configAccelerometer();
        instance.sensor.enableAccelerometer();
    }
#endif // SENSOR
#endif // ARDUINO
}

void hw_get_imu_diag(imu_diag_t &out)
{
    out = s_imu_diag;
}

void hw_unregister_imu_process()
{
#if defined(ARDUINO)
#if defined(USING_BHI260_SENSOR)
    if (hw_get_device_online() & HW_BHI260AP_ONLINE) {
        instance.sensor.configure(SensorBHI260AP::GAME_ROTATION_VECTOR, 0, 0);
    }
#elif defined(USING_BMA423_SENSOR)
    if (hw_get_device_online() & HW_BMA423_ONLINE) {
        instance.sensor.disableAccelerometer();
    }
#endif // SENSOR
#endif // ARDUINO
}

// --- Magnetometer ------------------------------------------------------

#ifdef USING_MAG_QMC5883
void hw_mag_enable(bool enable)
{
#ifdef ARDUINO
    if (enable) {
        /* Config Magnetometer */
        instance.mag.configMagnetometer(SensorQSTMagnetic::MODE_CONTINUOUS,
                                        SensorQSTMagnetic::RANGE_8G,
                                        SensorQSTMagnetic::DATARATE_100HZ,
                                        SensorQSTMagnetic::OSR_1,
                                        SensorQSTMagnetic::DSR_1);
    } else {
        instance.mag.setMode(SensorQSTMagnetic::MODE_SUSPEND);
    }
#endif // ARDUINO
}

float hw_mag_get_polar()
{
#ifdef ARDUINO
    Polar polar;
    if (instance.mag.readPolar(polar)) {
        return polar.polar;
    }
    return 0.0f;
#else
    static float sim_angle = 0;
    sim_angle = fmod(sim_angle + 0.5, 360);
    return sim_angle;
#endif
}

#endif // USING_MAG_QMC5883

// --- BME280 ------------------------------------------------------------

#ifdef USING_BME280

void hw_bme_enable(bool enable)
{
#ifdef ARDUINO
    if (enable) {
        instance.bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                                 Adafruit_BME280::SAMPLING_X1,   // temperature
                                 Adafruit_BME280::SAMPLING_X1, // pressure
                                 Adafruit_BME280::SAMPLING_X1,   // humidity
                                 Adafruit_BME280::FILTER_X2 );
    } else {
        instance.bme.setSampling(Adafruit_BME280::MODE_SLEEP);
    }
#endif
}


void hw_bme_get_data(float &temp, float &humi, float &press, float &alt)
{
#ifdef ARDUINO
    temp = instance.bme.readTemperature();
    humi = instance.bme.readHumidity();
    press = instance.bme.readPressure() / 100.0F;
    alt = instance.bme.readAltitude(1013.25);

#else
    temp = random(0, 25);
    humi = random(40, 95);
    press = random(1000, 1200);
    alt = random(20, 60);
#endif
}

#endif /*USING_BME280*/
