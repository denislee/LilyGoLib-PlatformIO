/**
 * @file      sensors.cpp
 * @brief     GPS, IMU, magnetometer, BME280.
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

static bool      pps_trigger = false;
static bool      sync_date_time = false;

void hw_gps_attach_pps()
{
#ifdef GPS_PPS
    pinMode(GPS_PPS, INPUT);
    attachInterrupt(GPS_PPS, []() {
        pps_trigger ^= 1;
    }, CHANGE);
#endif
}

void hw_gps_detach_pps()
{
#ifdef GPS_PPS
    detachInterrupt(GPS_PPS);
    pinMode(GPS_PPS, OPEN_DRAIN);
#endif
}

bool hw_get_gps_info(gps_params_t &param)
{
#ifdef ARDUINO
    static uint32_t last_update = 0;
    param.pps = pps_trigger;

    bool debug = param.enable_debug;

    if ((millis() - last_update) < 1000 && debug == false) {
        return false;
    }
    last_update = millis();

    param = gps_params_t{};


    param.model = instance.gps.getModel().c_str();
    param.rx_size = instance.gps.loop(debug);

    if (debug) {
        return false;
    }

    bool location = instance.gps.location.isValid();
    bool datetime = (instance.gps.date.year() > 2000);

    if (location) {
        param.lat = instance.gps.location.lat();
        param.lng = instance.gps.location.lng();
        param.speed = instance.gps.speed.kmph();
    }

    if (datetime) {
        if (!sync_date_time) {
            sync_date_time = true;
            struct tm utc_tm = {0};
            time_t utc_timestamp;
            struct tm *local_tm;
            utc_tm.tm_year = instance.gps.date.year() - 1900;
            utc_tm.tm_mon = instance.gps.date.month() - 1;
            utc_tm.tm_mday = instance.gps.date.day();
            utc_tm.tm_hour = instance.gps.time.hour();
            utc_tm.tm_min = instance.gps.time.minute();
            utc_tm.tm_sec = instance.gps.time.second();
            if (hw_get_device_online() & HW_RTC_ONLINE) {
                instance.rtc.convertUtcToTimezone(utc_tm, GMT_OFFSET_SECOND);
                instance.rtc.setDateTime(utc_tm);
                instance.rtc.hwClockRead();
            }
        }
        param.datetime.tm_year = instance.gps.date.year() - 1900;
        param.datetime.tm_mon = instance.gps.date.month() - 1;
        param.datetime.tm_mday = instance.gps.date.day();
        param.datetime.tm_hour = instance.gps.time.hour();
        param.datetime.tm_min =  instance.gps.time.minute();
        param.datetime.tm_sec = instance.gps.time.second();
    }

    if (instance.gps.satellites.isValid()) {
        param.satellite = instance.gps.satellites.value();
    }

    return location && datetime;
#else
    param.model = "Dummy";
    param.lat = 0.0;
    param.lng = 0.0;
    param.speed = rand() % 120;
    param.rx_size = 366666;
    time_t now;
    struct tm *timeinfo;
    time(&now);
    timeinfo  = localtime(&now);
    param.datetime = *timeinfo;
    param.satellite = rand() % 30;
    return true;
#endif
}

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

#if  defined(ARDUINO) && defined(USING_BHI260_SENSOR)
static void imu_data_process(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp, void *user_data)
{
    float roll, pitch, yaw;
    bhy2_quaternion_to_euler(data_ptr, &roll,  &pitch, &yaw);
    imu_params.roll = roll;
    imu_params.pitch = pitch;
    imu_params.heading = yaw;
}
#endif //ARDUINO

void hw_register_imu_process()
{
#if defined(ARDUINO)
#if defined(USING_BHI260_SENSOR)
    if (hw_get_device_online() & HW_BHI260AP_ONLINE) {
        float sample_rate = 100.0;      /* Read out data measured at 100Hz */
        uint32_t report_latency_ms = 0; /* Report immediately */
        // LilyGoLib has already processed it
        // instance.sensor.setRemapAxes(SensorBHI260AP::BOTTOM_LAYER_TOP_LEFT_CORNER);
        // Enable Quaternion function
        instance.sensor.configure(SensorBHI260AP::GAME_ROTATION_VECTOR, sample_rate, report_latency_ms);
        // Register event callback function
        instance.sensor.onResultEvent(SensorBHI260AP::GAME_ROTATION_VECTOR, imu_data_process);
    }
#elif defined(USING_BMA423_SENSOR)
    if (hw_get_device_online() & HW_BMA423_ONLINE) {
        instance.sensor.configAccelerometer();
        instance.sensor.enableAccelerometer();
    }
#endif // SENSOR
#endif // ARDUINO
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
