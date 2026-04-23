/**
 * @file      factory.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-04
 *
 */
#ifdef ARDUINO
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include "hal_interface.h"
#include "event_define.h"

static const char *ntpServer1 = "pool.ntp.org";
static const char *ntpServer2 = "time.nist.gov";
static const uint64_t  gmtOffset_sec = GMT_OFFSET_SECOND;
static const int   daylightOffset_sec = 0;
static SemaphoreHandle_t xSemaphore = NULL;


void instanceLockTake()
{
    if (xSemaphore != NULL) {
        if (xSemaphoreTake(xSemaphore, portMAX_DELAY) != pdTRUE) {
            log_e("Failed to take semaphore");
            assert(0);
        }
    }
}

void instanceLockGive()
{
    if (xSemaphore != NULL) {
        if (xSemaphoreGive(xSemaphore) != pdTRUE) {
            log_e("Failed to give semaphore");
            assert(0);
        }
    }
}

// Callback function (gets called when time adjusts via NTP)
static void time_available(struct timeval *t)
{
    Serial.println("Got time adjustment from NTP!");
    // printLocalTime();
    if (instance.getDeviceProbe() & HW_RTC_ONLINE) {
        instance.rtc.hwClockWrite();
    }
    hw_notify_time_sync_completed();
}

// WARNING: This function is called from a separate FreeRTOS task (thread)!
void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(IPAddress(info.got_ip.ip_info.ip.addr));
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
}

#include "core/system.h"
#include "core/scoped_lock.h"
#include "apps/app_registry.h"
#include "hal/lvgl_task.h"
#include "hal/nfc_task.h"

void setup()
{
    setCpuFrequencyMhz(240);

    Serial.begin(115200);

    xSemaphore = xSemaphoreCreateMutex();
    if (xSemaphore == NULL) {
        log_e("Failed to create mutex");
        assert(0);
    }

    hw_load_setting();
    user_setting_params_t settings;
    hw_get_user_setting(settings);

    setCpuFrequencyMhz(settings.cpu_freq_mhz);

    uint32_t disable_flags = 0;
    if (!settings.gps_enable) disable_flags |= NO_HW_GPS;
    if (!settings.nfc_enable) disable_flags |= NO_HW_NFC;
    if (!settings.haptic_enable) disable_flags |= NO_HW_DRV;

    instance.begin(disable_flags);

    beginLvglHelper(instance);

    hw_init();

    apps::register_all();
    core::System::getInstance().init();

    // Defer WiFi init until after GUI is showing
    sntp_set_time_sync_notification_cb(time_available);
    if (hw_get_wifi_enable()) {
        WiFi.mode(WIFI_STA);
    } else {
        WiFi.mode(WIFI_OFF);
    }
    WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.setAutoReconnect(false);

    // LVGL rendering and NFC polling now run on their own FreeRTOS tasks so
    // the main loop's mutex hold window stays short. Must come after
    // core::System::getInstance().init() — the LVGL task calls System::loop().
    hw_lvgl_task_start();
    hw_nfc_task_start();

    Serial.println("Start done. run main loop");
}

extern bool ui_is_fake_sleep();

void loop()
{
    uint32_t inactive_time = 0;
    {
        core::ScopedInstanceLock lock;
        instance.loop();
        if (!ui_is_fake_sleep()) {
            inactive_time = lv_display_get_inactive_time(NULL);
        }
    }

    static uint32_t last_freq = 0;
    if (ui_is_fake_sleep()) {
        // BLE and WiFi both need ≥80MHz; hold there while either link is
        // up so the fake-sleep power saving doesn't drop them.
        bool hold_80 = hw_get_ble_kb_connected() || hw_get_wifi_connected();
        uint32_t fake_sleep_freq = hold_80 ? 80 : 40;
        if (last_freq != fake_sleep_freq) {
            setCpuFrequencyMhz(fake_sleep_freq);
            last_freq = fake_sleep_freq;
        }
    } else {
        user_setting_params_t settings;
        hw_get_user_setting(settings);
        uint32_t active_freq = settings.cpu_freq_mhz;

        if (inactive_time > 2000 && active_freq > 80) {
            if (last_freq != 80) {
                setCpuFrequencyMhz(80);
                last_freq = 80;
            }
        } else {
            if (last_freq != active_freq) {
                setCpuFrequencyMhz(active_freq);
                last_freq = active_freq;
            }
        }
    }

    delay(50);
}

#endif
