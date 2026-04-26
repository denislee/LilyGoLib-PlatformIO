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
#include "hal/notes_crypto.h"
#include "event_define.h"
#include "core/system_hooks.h"

#include <string>

std::string timezone_get_user_tz();
bool timezone_fetch_offset(const char *tz, int &raw_offset_sec, int &dst_offset_sec, std::string &err);



/* Defined in ui_lock.cpp — shows the unlock modal if crypto is enabled and
 * the session is locked. No-op otherwise. */
void ui_device_lock_enforce();

static const char *ntpServer1 = "pool.ntp.org";
static const char *ntpServer2 = "time.nist.gov";
static const uint64_t  gmtOffset_sec = GMT_OFFSET_SECOND;
static const int   daylightOffset_sec = 0;

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
    /* NTP sync is driven from the main loop's poll (see loop()) rather than
     * from this event callback, so boot-time flows that connect WiFi before
     * the event handler is fully wired are still covered. */
}

#include "core/system.h"
#include "apps/app_registry.h"
#include "hal/lvgl_task.h"
#include "hal/nfc_task.h"
#include "hal/rotary_task.h"
#include "hal/charge_task.h"

void setup()
{
    setCpuFrequencyMhz(240);

    Serial.begin(115200);

    core::instance_lock_init();

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

    // Arm the SNTP and WiFi event callbacks BEFORE hw_init() — hw_init() calls
    // hw_set_wifi_enable() which in turn fires WiFi.begin() from saved
    // credentials. Registering the notification callback after WiFi.begin()
    // races the first DHCP/GOT_IP event and can miss the sync that kicks off
    // on cold boot, leaving the clock stuck at 1970.
    sntp_set_time_sync_notification_cb(time_available);
    WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.setAutoReconnect(false);

    hw_init();

    apps::register_all();
    core::System::getInstance().init();

    // Passphrase-protected notes: if the session is locked, put an unlock
    // modal on top of the UI before rendering starts. The modal has no cancel
    // button so the device stays gated until the user enters the passphrase.
    ui_device_lock_enforce();

    // LVGL rendering and NFC polling now run on their own FreeRTOS tasks so
    // the main loop's mutex hold window stays short. Must come after
    // core::System::getInstance().init() — the LVGL task calls System::loop().
    hw_lvgl_task_start();
    hw_nfc_task_start();
    hw_rotary_task_start();
    hw_charge_task_start();

    Serial.println("Start done. run main loop");
}

void loop()
{
    // Poll-based NTP driver: keep re-triggering configTime() while WiFi is
    // up and the clock has not actually synced yet. A single trigger is not
    // enough — lwIP's SNTP will retry on its own, but on a cold boot the
    // first DNS lookup or UDP round-trip often drops (AP still handshaking,
    // DHCP lease just arrived, etc.) and we want to force a fresh cycle
    // until the time_available() callback fires. Re-triggers every 30 s so
    // we don't thrash the tcpip task or the NTP pool.
    static uint32_t last_ntp_attempt_ms = 0;
    if (hw_get_time_sync_status() == 0 && hw_get_wifi_connected()) {
        uint32_t now = millis();
        if (last_ntp_attempt_ms == 0 || now - last_ntp_attempt_ms > 30000) {
            Serial.println("Triggering NTP sync");
            std::string tz = timezone_get_user_tz();
            int offset_sec = 0;
            if (!tz.empty()) {
                int raw = 0, dst = 0;
                std::string err;
                if (timezone_fetch_offset(tz.c_str(), raw, dst, err)) {
                    offset_sec = raw + dst;
                    hw_start_time_sync_ntp(offset_sec);
                } else {
                    hw_start_time_sync_ntp();
                }
            } else {
                hw_start_time_sync_ntp();
            }
            last_ntp_attempt_ms = now;
        }
    }

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
