/**
 * @file      system.cpp
 * @brief     System init, settings persistence, device info, sleep/shutdown,
 *            date/time, feedback, and the shared HAL state definitions.
 */
#include "../hal_interface.h"   // umbrella: pulls all hal/ headers and `using std::string`.
#include "internal.h"

#include <cstring>
#include <lvgl.h>


#define NVS_NAME    "pager"
user_setting_params_t user_setting;

// Settings blob schema guard.
//
// The stored NVS blob is a `SettingsHeader` followed by a raw
// `user_setting_params_t`. The header lets us reject bytes left over from a
// previous schema instead of silently re-interpreting them as the new one.
//
// *** Bump SETTINGS_VERSION whenever `user_setting_params_t` changes in a way
//     that is not byte-compatible (field reorder, type change, field removed).
//     Adding a new field to the *end* keeps the size check effective and is
//     safe without a version bump — missing bytes keep their default values.
static constexpr uint32_t SETTINGS_MAGIC   = 0x50414752u; // "PAGR"
static constexpr uint16_t SETTINGS_VERSION = 2;

struct SettingsHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size;
};

#ifdef ARDUINO

#include "Esp.h"
#include <LilyGoLib.h>
#include <esp_mac.h>
#include <Preferences.h>
#include "driver/rtc_io.h"

static Preferences           prefs;

void save_user_setting_nvs()
{
    uint8_t buf[sizeof(SettingsHeader) + sizeof(user_setting_params_t)];
    SettingsHeader h{SETTINGS_MAGIC, SETTINGS_VERSION, (uint16_t)sizeof(user_setting_params_t)};
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &user_setting, sizeof(user_setting_params_t));
    prefs.putBytes(NVS_NAME, buf, sizeof(buf));
}

#endif

device_const_var_t dev_conts_var = {
    .max_brightness = DEVICE_MAX_BRIGHTNESS_LEVEL,
    .min_brightness = DEVICE_MIN_BRIGHTNESS_LEVEL,
    .max_charge_current = DEVICE_MAX_CHARGE_CURRENT,
    .min_charge_current = DEVICE_MIN_CHARGE_CURRENT,
    .charge_level_nums = DEVICE_CHARGE_LEVEL_NUMS,
    .charge_steps = DEVICE_CHARGE_STEPS,
};


static const char *hw_devices[] = {
    USING_RADIO_NAME,

#ifdef USING_INPUT_DEV_TOUCHPAD
    "Touch Panel",
#else
    "",
#endif
    "Haptic Drive",
    "Power management",
    "Real-time clock",
    "PSRAM",
    "GPS",
#ifdef HAS_SD_CARD_SOCKET
    "SD card",
#else
    "",
#endif
#ifdef USING_ST25R3916
    "NFC",
#else
    "",
#endif

#ifdef USING_BHI260_SENSOR
    "BHI260AP 6-Axis Sensor",
#else
    "",
#endif
#ifdef USING_INPUT_DEV_KEYBOARD
    "Keyboard",
#else
    "",
#endif

#ifdef USING_BQ_GAUGE
    "Gauge",
#else
    "",
#endif

#ifdef USING_XL9555_EXPANDS
    "Expands Control",
#else
    "",
#endif

#ifdef USING_AUDIO_CODEC
    "Audio codec",
#else
    "",
#endif

#ifdef USING_EXTERN_NRF2401
    "NRF2401 Sub 1G",
#else
    "",
#endif

#ifdef USING_SI473X_RADIO
    "SI4735 Radio",
#else
    "",
#endif

#ifdef USING_BME280
    "BME280 Pressure & Temperature",
#else
    "",
#endif

#ifdef USING_MAG_QMC5883
    "QMC5883P Magnetometer",
#else
    "",
#endif

#ifdef USING_BMA423_SENSOR
    "BMA423 Accelerometer",
#else
    "",
#endif

#ifdef USING_QMI8658_SENSOR
    "QMI8658 6-Axis Sensor",
#else
    "",
#endif

};


extern void hw_nrf24_begin();
extern void hw_radio_begin();


#ifndef ARDUINO
static time_t emu_time_offset = 0;

int random(int min, int max)
{
    if (min > max) {
        int temp = min;
        min = max;
        max = temp;
    }
    int range = max - min + 1;
    return rand() % range + min;
}
#endif


#ifdef ARDUINO

size_t getArduinoLoopTaskStackSize(void)
{
    return 30 * 1024;
}

#endif






#ifdef ARDUINO_T_LORA_PAGER
const uint8_t mic_gain = 10;
#else
const uint8_t mic_gain = 10;
#endif


void hw_init()
{
#ifdef ARDUINO
    if (instance.getDeviceProbe() & HW_RTC_ONLINE) {
        struct tm timeinfo;
        instance.rtc.getDateTime(&timeinfo);
        struct timeval tv;
        tv.tv_sec = mktime(&timeinfo);
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);
        log_i("System time synchronized with RTC");
    }

    hw_audio_init();

    hw_radio_begin();
#ifdef USING_EXTERN_NRF2401
    hw_nrf24_begin();
#endif


#ifdef USING_AUDIO_CODEC
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        instance.codec.setVolume(100);
        instance.codec.setGain(mic_gain);
    } else {
        log_w("Audio codec not online!");
    }
#endif //USING_AUDIO_CODEC

#ifdef USING_INPUT_DEV_KEYBOARD
    instance.attachKeyboardFeedback(false, 0);

    instance.setFeedbackCallback([](void *args) {
        // Feedback disabled
    });
#endif //USING_INPUT_DEV_KEYBOARD


#endif

    hw_load_setting();

#ifdef ARDUINO
    hw_set_charger(user_setting.charger_enable);
    hw_set_charger_current(user_setting.charger_current);
    // Ensure WiFi, BT and Radio match settings on boot
    hw_set_wifi_enable(user_setting.wifi_enable);
    instance.setMSCPreferSD(user_setting.msc_prefer_sd != 0);
    hw_set_bt_enable(user_setting.bt_enable);
    hw_set_radio_enable(user_setting.radio_enable);
    hw_set_nfc_enable(user_setting.nfc_enable);
    hw_set_gps_enable(user_setting.gps_enable);
    hw_set_speaker_enable(user_setting.speaker_enable);
    hw_set_haptic_enable(user_setting.haptic_enable);
#endif

    // Battery history recording every 1 minute (60,000 ms)
    lv_timer_create(battery_history_timer_cb, 60000, NULL);
    // Record first point immediately - This also triggers the 80% charge limit check
    hw_update_battery_history();

#ifdef ARDUINO
    hw_set_disp_backlight(user_setting.brightness_level);

    hw_set_kb_backlight(user_setting.keyboard_bl_level);

    instance.onEvent([](DeviceEvent_t event, void *params, void *user_data) {
        int pmu_event = instance.getPMUEventType(params);
        if (pmu_event == PMU_EVENT_KEY_CLICKED) {
            log_d("ON EVENT PMU CLICK");
        } else if (pmu_event == PMU_EVENT_KEY_LONG_PRESSED) {
            log_d("ON EVENT PMU LONG CLICK -> Sleep");
            hw_low_power_loop();
        }
    }, POWER_EVENT, NULL);
#endif

}

void hw_load_setting()
{
#ifdef ARDUINO
    // Initialize with defaults first
    user_setting.brightness_level = 50;
    user_setting.keyboard_bl_level = 80;
    user_setting.disp_timeout_second = 0;
    user_setting.charger_current = DEVICE_CHARGE_CURRENT_RECOMMEND;
    user_setting.charger_enable = true;
    user_setting.sleep_mode = 0;
    user_setting.editor_font_size = 14;
    user_setting.editor_font_index = 0;
    user_setting.charge_limit_en = false;
    user_setting.wifi_enable = 0;
    user_setting.bt_enable = 0;
    user_setting.radio_enable = 0;
    user_setting.nfc_enable = 0;
    user_setting.gps_enable = 0;
    user_setting.speaker_enable = 0;
    user_setting.haptic_enable = 1;
    user_setting.show_mem_usage = 0;
    user_setting.cpu_freq_mhz = 240;
    user_setting.storage_prefer_sd = 0;
    user_setting.msc_prefer_sd = 0;
    user_setting.prune_internal = 0;

    prefs.begin(NVS_NAME);
    const size_t stored_size = prefs.getBytesLength(NVS_NAME);
    constexpr size_t kVersionedSize = sizeof(SettingsHeader) + sizeof(user_setting_params_t);

    bool loaded = false;

    if (stored_size == kVersionedSize) {
        uint8_t buf[kVersionedSize];
        prefs.getBytes(NVS_NAME, buf, sizeof(buf));
        const SettingsHeader *h = reinterpret_cast<const SettingsHeader *>(buf);
        if (h->magic == SETTINGS_MAGIC &&
            h->version == SETTINGS_VERSION &&
            h->payload_size == sizeof(user_setting_params_t)) {
            memcpy(&user_setting, buf + sizeof(SettingsHeader), sizeof(user_setting_params_t));
            loaded = true;
        } else {
            log_i("Settings header mismatch (magic=%08x ver=%u size=%u), resetting to defaults",
                  (unsigned)h->magic, (unsigned)h->version, (unsigned)h->payload_size);
        }
    } else if (stored_size > 0 && stored_size <= sizeof(user_setting_params_t)) {
        // Legacy unversioned blob: copy what fits onto defaults, then re-save with header.
        prefs.getBytes(NVS_NAME, &user_setting, stored_size);
        log_i("Migrating legacy settings blob (%u bytes) to versioned format", (unsigned)stored_size);
        loaded = true;
    } else if (stored_size != 0) {
        log_i("Stored settings size %u unrecognized, resetting to defaults", (unsigned)stored_size);
    } else {
        log_i("No stored settings found, using defaults");
    }
    (void)loaded;
    // Always persist in the current format so subsequent boots take the fast path.
    save_user_setting_nvs();
#else
    user_setting.brightness_level = 10;
    user_setting.keyboard_bl_level = 255;
    user_setting.disp_timeout_second = 0;
    user_setting.charger_current = 1000;
    user_setting.charger_enable = true;
    user_setting.nfc_enable = 0;
    user_setting.gps_enable = 0;
    user_setting.speaker_enable = 0;
    user_setting.haptic_enable = 1;
#endif
}

void hw_get_user_setting(user_setting_params_t &param)
{
    param = user_setting;
}

void hw_set_user_setting(user_setting_params_t &param)
{
    user_setting = param;
#ifdef ARDUINO
    save_user_setting_nvs();
#endif
}



bool hw_get_haptic_enable() { return user_setting.haptic_enable; }
void hw_set_haptic_enable(bool en) {
    user_setting.haptic_enable = en;
#ifdef ARDUINO
    instance.powerControl(POWER_HAPTIC_DRIVER, en);
    delay(10);
#endif
}

uint16_t hw_get_devices_nums()
{
    return sizeof(hw_devices) / sizeof(hw_devices[0]);
}

const char *hw_get_devices_name(int index)
{
    if (index >= hw_get_devices_nums()) {
        return "NULL";
    }
    return hw_devices[index];
}

const char *hw_get_variant_name()
{
#ifdef ARDUINO
    return instance.getName();
#else
    return "LilyGo T-LoRa-Pager (2025)";
#endif
}


bool hw_get_mac(uint8_t *mac)
{
#ifdef ARDUINO
    esp_efuse_mac_get_default(mac);
    return true;
#endif
    return false;
}

void hw_get_date_time(string &param)
{
#ifdef ARDUINO
    struct tm timeinfo;
    if (hw_get_device_online() & HW_RTC_ONLINE) {
        instance.rtc.getDateTime(&timeinfo);
        char datetime[128] = {0};
        snprintf(datetime, 128, "%04d/%02d/%02d %02d:%02d:%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        param  = datetime;
    } else {
        param = "2000/01/01 00:00:00";
    }
#else
    time_t now;
    struct tm *timeinfo;
    time(&now);
    now += emu_time_offset;
    timeinfo = localtime(&now);
    char datetime[128] = {0};
    snprintf(datetime, 128, "%04d/%02d/%02d %02d:%02d:%02d",
             timeinfo->tm_year + 1900,
             timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour,
             timeinfo->tm_min,
             timeinfo->tm_sec);
    param  = datetime;
#endif
}

void hw_get_date_time(struct tm &timeinfo)
{
#ifdef ARDUINO
    if (hw_get_device_online() & HW_RTC_ONLINE) {
        instance.rtc.getDateTime(&timeinfo);
    } else {
        time_t now;
        time(&now);
        localtime_r(&now, &timeinfo);
    }
#else
    time_t now;
    time(&now);
    now += emu_time_offset;
    timeinfo = *localtime(&now);
#endif
}

void hw_set_date_time(struct tm &timeinfo)
{
#ifdef ARDUINO
    struct timeval tv;
    tv.tv_sec = mktime(&timeinfo);
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    if (hw_get_device_online() & HW_RTC_ONLINE) {
        instance.rtc.setDateTime(timeinfo);
        instance.rtc.hwClockWrite();
    }
#else
    time_t target = mktime(&timeinfo);
    time_t now;
    time(&now);
    emu_time_offset = target - now;
#endif
}



void hw_get_arduino_version(string &param)
{
#ifdef ARDUINO
    param.clear();
    param.append("V");
    param.append(std::to_string(ESP_ARDUINO_VERSION_MAJOR));
    param.append(".");
    param.append(std::to_string(ESP_ARDUINO_VERSION_MINOR));
    param.append(".");
    param.append(std::to_string(ESP_ARDUINO_VERSION_PATCH));
#else
    param = "V2.0.17";
#endif
}


uint32_t hw_get_device_online()
{
#ifdef ARDUINO
    return instance.getDeviceProbe();
#else
    uint32_t hw_online =   HW_TOUCH_ONLINE | HW_DRV_ONLINE | HW_PMU_ONLINE;
#ifdef USING_INPUT_DEV_KEYBOARD
    hw_online |= HW_KEYBOARD_ONLINE;
#endif
    return hw_online;
#endif
}





void hw_shutdown()
{
#ifdef ARDUINO
    instance.decrementBrightness(0, 5, false);
#if defined(USING_PPM_MANAGE)
    instance.ppm.shutdown();
#elif defined(USING_PMU_MANAGE)
    instance.pmu.shutdown();
#endif
#endif
}

void hw_light_sleep()
{
#ifdef ARDUINO
    instance.decrementBrightness(0, 5, false);
    instance.lightSleep((WakeupSource_t)(WAKEUP_SRC_BOOT_BUTTON));
#endif
}

void hw_power_down_all()
{
#ifdef ARDUINO
    instance.powerControl(POWER_GPS, false);
    instance.powerControl(POWER_NFC, false);
    instance.powerControl(POWER_HAPTIC_DRIVER, false);
    instance.powerControl(POWER_SPEAK, false);
    instance.powerControl(POWER_KEYBOARD, false);
    hw_disable_keyboard();
    // SD Card is left on to avoid mount/unmount overhead and potential filesystem issues
    
    // Lower CPU frequency to 40MHz for power saving during fake sleep
    setCpuFrequencyMhz(40);
#endif
}

void hw_power_up_all()
{
#ifdef ARDUINO
    // Revert CPU frequency to user set value
    setCpuFrequencyMhz(user_setting.cpu_freq_mhz);

    if (user_setting.gps_enable) instance.powerControl(POWER_GPS, true);
    if (user_setting.nfc_enable) instance.powerControl(POWER_NFC, true);
    if (user_setting.haptic_enable) instance.powerControl(POWER_HAPTIC_DRIVER, true);
    instance.powerControl(POWER_KEYBOARD, true);
    hw_enable_keyboard();
    // Speaker is only powered on when needed by playback routines and if enabled
#endif
}

void hw_sleep()
{
#ifdef ARDUINO
    hw_audio_deinit_task();

#ifdef USING_PDM_MICROPHONE
    instance.mic.end();
#endif

#ifdef USING_PCM_AMPLIFIER
    instance.player.end();
#endif

    instance.decrementBrightness(0, 5, false);
    instance.sleep((WakeupSource_t)(WAKEUP_SRC_BOOT_BUTTON));
#endif
}

void hw_feedback()
{
#ifdef ARDUINO
    instance.vibrator();
#endif
}

void hw_low_power_loop()
{
#ifdef ARDUINO
    if (user_setting.sleep_mode == 1) {
        hw_sleep();
    } else {
        instance.lightSleep((WakeupSource_t)(WAKEUP_SRC_BOOT_BUTTON));
    }
#endif
}

void hw_set_cpu_freq(uint32_t mhz)
{
#ifdef ARDUINO
    setCpuFrequencyMhz(mhz);
#endif
}

void hw_disable_input_devices()
{
#if defined(ARDUINO) && defined(USING_INPUT_DEV_ROTARY)
    instance.disableRotary();
#endif
}


void hw_enable_input_devices()
{
#if defined(ARDUINO) && defined(USING_INPUT_DEV_ROTARY)
    instance.enableRotary();
#endif
}


#if defined(ARDUINO)
#include <Esp.h>
#endif
void hw_print_mem_info()
{
#if defined(ARDUINO)
    printf("INTERNAL Memory Info:\n");
    printf("------------------------------------------\n");
    printf("  Total Size        :   %u B ( %.1f KB)\n", ESP.getHeapSize(), ESP.getHeapSize() / 1024.0);
    printf("  Free Bytes        :   %u B ( %.1f KB)\n", ESP.getFreeHeap(), ESP.getFreeHeap() / 1024.0);
    printf("  Minimum Free Bytes:   %u B ( %.1f KB)\n", ESP.getMinFreeHeap(), ESP.getMinFreeHeap() / 1024.0);
    printf("  Largest Free Block:   %u B ( %.1f KB)\n", ESP.getMaxAllocHeap(), ESP.getMaxAllocHeap() / 1024.0);
    printf("------------------------------------------\n");
    printf("SPIRAM Memory Info:\n");
    printf("------------------------------------------\n");
    printf("  Total Size        :  %u B (%.1f KB)\n", ESP.getPsramSize(), ESP.getPsramSize() / 1024.0);
    printf("  Free Bytes        :  %u B (%.1f KB)\n", ESP.getFreePsram(), ESP.getFreePsram() / 1024.0);
    printf("  Minimum Free Bytes:  %u B (%.1f KB)\n", ESP.getMinFreePsram(), ESP.getMinFreePsram() / 1024.0);
    printf("  Largest Free Block:  %u B (%.1f KB)\n", ESP.getMaxAllocPsram(), ESP.getMaxAllocPsram() / 1024.0);
    printf("------------------------------------------\n");
#endif
}

void hw_get_heap_info(uint32_t &total, uint32_t &free)
{
#if defined(ARDUINO)
    total = ESP.getHeapSize();
    free = ESP.getFreeHeap();
#else
    total = 512 * 1024;
    free = 256 * 1024;
#endif
}




using TrackballEventCallback = void(*)(uint8_t dir);
using ButtonEventCallback = void(*)(uint8_t idx, uint8_t state);

#if defined(ARDUINO) && defined(USING_TRACKBALL)

static TrackballEventCallback _trackball_cb = NULL;
static ButtonEventCallback    _button_cb = NULL;


static void trackballEventCallback(DeviceEvent_t event, void *params, void *user_data)
{
    if (_trackball_cb && params) {
        TrackballDir_t dir = *(static_cast < TrackballDir_t * > (params));
        _trackball_cb(dir);
    }
}

static void buttonEventCallback(DeviceEvent_t event, void *params, void *user_data)
{
    if (_button_cb && params) {
        ButtonEventParam_t *p = static_cast < ButtonEventParam_t * > (params);
        _button_cb(p->id, p->event);
    }
}

#endif

const char *hw_get_firmware_hash_string()
{
#ifdef ARDUINO
    static char hash_string[33] = {0};
    snprintf(hash_string, sizeof(hash_string), "%s", ESP.getSketchMD5().c_str());
    return hash_string;
#else
    return "DummyHashString";
#endif
}

const char *hw_get_chip_id_string()
{
#ifdef ARDUINO
    static char chipid[13] = {0};
    uint64_t chipmacid = 0LL;
    esp_efuse_mac_get_default((uint8_t *)(&chipmacid));
    snprintf(chipid, sizeof(chipid), "%04X%08X", (uint16_t)(chipmacid >> 32), (uint32_t)(chipmacid));
    return chipid;
#endif
    return "DummyChipIDString";
}


