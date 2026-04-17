/**
 * @file      types.h
 * @brief     Shared HAL data types, enums, and constants.
 *
 * All domain HAL headers (system, display, power, ...) include this file.
 * Keep it free of function declarations.
 */
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <string>
#include <vector>
#include "../event_define.h"

// Debug logging macro - define DEBUG_RADIO to enable verbose radio output
#ifdef DEBUG_RADIO
#define RADIO_LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define RADIO_LOG(fmt, ...) ((void)0)
#endif

typedef enum {
    MEDIA_VOLUME_UP,
    MEDIA_VOLUME_DOWN,
    MEDIA_PLAY_PAUSE,
    MEDIA_NEXT,
    MEDIA_PREVIOUS
} media_key_value_t;

typedef enum {
    KEYBOARD_TYPE_NONE,
    KEYBOARD_TYPE_1,
    KEYBOARD_TYPE_2,
} keyboard_type_t;

/* Radio frequency constants */
#define RADIO_DEFAULT_FREQUENCY  868.0

#ifndef ARDUINO

#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))

#ifndef _BV
#define _BV(x)                      (1UL<<x)
#endif

/**
 * @brief Enumeration representing different WiFi statuses.
 *
 * Mirrors the Arduino WiFi Shield library for host-side (emulator) builds.
 */
typedef enum {
    WL_NO_SHIELD = 255,
    WL_STOPPED = 254,
    WL_IDLE_STATUS = 0,
    WL_NO_SSID_AVAIL = 1,
    WL_SCAN_COMPLETED = 2,
    WL_CONNECTED = 3,
    WL_CONNECT_FAILED = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED = 6
} wl_status_t;

#define DEVICE_MAX_BRIGHTNESS_LEVEL 255
#define DEVICE_MIN_BRIGHTNESS_LEVEL 0
#define DEVICE_MAX_CHARGE_CURRENT   1000
#define DEVICE_MIN_CHARGE_CURRENT   100
#define DEVICE_CHARGE_LEVEL_NUMS    12
#define DEVICE_CHARGE_STEPS         1
#define USING_RADIO_NAME            "SX12XX"

// Hardware online status bit definitions
#define HW_RADIO_ONLINE             (_BV(0))
#define HW_TOUCH_ONLINE             (_BV(1))
#define HW_DRV_ONLINE               (_BV(2))
#define HW_PMU_ONLINE               (_BV(3))
#define HW_RTC_ONLINE               (_BV(4))
#define HW_PSRAM_ONLINE             (_BV(5))
#define HW_GPS_ONLINE               (_BV(6))
#define HW_SD_ONLINE                (_BV(7))
#define HW_NFC_ONLINE               (_BV(8))
#define HW_BHI260AP_ONLINE          (_BV(9))
#define HW_KEYBOARD_ONLINE          (_BV(10))
#define HW_GAUGE_ONLINE             (_BV(11))
#define HW_EXPAND_ONLINE            (_BV(12))
#define HW_CODEC_ONLINE             (_BV(13))
#define HW_NRF24_ONLINE             (_BV(14))
#define HW_SI473X_ONLINE            (_BV(15))
#define HW_BME280_ONLINE            (_BV(16))
#define HW_QMC5883P_ONLINE          (_BV(17))
#define HW_BMA423_ONLINE            (_BV(18))
#define HW_QMI8658_ONLINE           (_BV(19))
#define HW_LED_INDIC_ONLINE         (_BV(20))

#else
#include <WiFi.h>
#endif

#define GMT_OFFSET_SECOND       (8*3600)

typedef struct {
    std::string model;
    double lat;
    double lng;
    struct tm datetime;
    double speed;
    uint32_t rx_size;
    uint16_t satellite;
    bool pps;
    bool enable_debug;
} gps_params_t;

enum RadioMode {
    RADIO_DISABLE,
    RADIO_TX,
    RADIO_RX,
    RADIO_CW,
};

typedef struct {
    bool isRunning;
    float freq;
    float bandwidth;
    uint16_t cr;
    uint8_t power;
    uint8_t sf;
    uint8_t mode;
    uint8_t syncWord;
    uint32_t interval;
} radio_params_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t authmode;
    int8_t  rssi;
    int32_t channel;
    std::string ssid;
} wifi_scan_params_t;

typedef struct {
    std::string ssid;
    std::string password;
} wifi_conn_params_t;

typedef enum {
    AUDIO_SOURCE_FATFS,
    AUDIO_SOURCE_SDCARD,
} audio_source_type_t;

typedef struct {
    audio_source_type_t source_type;
    std::string file_name;
} AudioParams_t;

typedef enum {
    MONITOR_PMU,
    MONITOR_PPM,
} monitor_params_type_t;

typedef struct {
    monitor_params_type_t type;
    std::string charge_state;
    uint16_t sys_voltage;
    uint16_t battery_voltage;
    uint16_t usb_voltage;
    int      battery_percent;
    float    temperature;
    uint16_t remainingCapacity;
    uint16_t fullChargeCapacity;
    uint16_t designCapacity;
    int16_t  instantaneousCurrent;
    int16_t  standbyCurrent;
    int16_t  averagePower;
    int16_t  maxLoadCurrent;
    uint16_t timeToEmpty;
    uint16_t timeToFull;
    std::string ntc_state;
    bool is_charging;
} monitor_params_t;

typedef struct {
    uint8_t brightness_level;
    uint8_t keyboard_bl_level;
    uint8_t led_indicator_level;
    uint8_t disp_timeout_second;
    uint16_t charger_current;
    uint8_t charger_enable;
    uint8_t sleep_mode;
    uint8_t editor_font_size;
    uint8_t editor_font_index;
    uint8_t charge_limit_en;
    uint8_t wifi_enable;
    uint8_t bt_enable;
    uint8_t radio_enable;
    uint8_t nfc_enable;
    uint8_t gps_enable;
    uint8_t speaker_enable;
    uint8_t haptic_enable;
    uint8_t show_mem_usage;
    uint16_t cpu_freq_mhz;
    uint8_t storage_prefer_sd;
    uint8_t msc_prefer_sd;
} user_setting_params_t;

typedef struct {
    enum app_event event;
    const char *filename;
    audio_source_type_t source_type;
} audio_params_t;

typedef struct {
    uint8_t *data;
    size_t  length;
    int state;
} radio_tx_params_t;

typedef struct {
    uint8_t *data;
    size_t  length;
    int16_t rssi;
    int16_t snr;
    int state;
} radio_rx_params_t;

typedef struct {
    float roll;
    float pitch;
    float heading;
    uint8_t orientation;
} imu_params_t;

typedef enum {
    HW_TRACKBALL_DIR_NONE,
    HW_TRACKBALL_DIR_UP,
    HW_TRACKBALL_DIR_DOWN,
    HW_TRACKBALL_DIR_LEFT,
    HW_TRACKBALL_DIR_RIGHT
} hw_trackball_dir;

// FFT Configuration
#define FFT_SIZE 512
#define SAMPLE_RATE 16000
#define FREQ_BANDS 16

typedef struct {
    float left_bands[FREQ_BANDS];
    float right_bands[FREQ_BANDS];
} FFTData;

enum Si4735Mode {
    FM,
    LSB,
    USB,
    AM,
};
