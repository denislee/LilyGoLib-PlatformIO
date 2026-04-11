# 1 "/tmp/tmp2sjxhdal"
#include <Arduino.h>
# 1 "/home/dns/tmp/LilyGoLib-PlatformIO/src/factory.ino"
# 9 "/home/dns/tmp/LilyGoLib-PlatformIO/src/factory.ino"
#ifdef ARDUINO
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include "hal_interface.h"
#include "event_define.h"

extern void setupGui();

static const char *ntpServer1 = "pool.ntp.org";
static const char *ntpServer2 = "time.nist.gov";
static const uint64_t gmtOffset_sec = GMT_OFFSET_SECOND;
static const int daylightOffset_sec = 0;
static SemaphoreHandle_t xSemaphore = NULL;
void instanceLockTake();
void instanceLockGive();
static void time_available(struct timeval *t);
void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
void setup();
void loop();
#line 26 "/home/dns/tmp/LilyGoLib-PlatformIO/src/factory.ino"
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


static void time_available(struct timeval *t)
{
    Serial.println("Got time adjustment from NTP!");

    if (instance.getDeviceProbe() & HW_RTC_ONLINE) {
        instance.rtc.hwClockWrite();
    }
}


void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(IPAddress(info.got_ip.ip_info.ip.addr));
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
}

void setup()
{
    setCpuFrequencyMhz(240);

    Serial.begin(115200);

    xSemaphore = xSemaphoreCreateMutex();
    if (xSemaphore == NULL) {
        log_e("Failed to create mutex");
        assert(0);
    }

    instance.begin();

    beginLvglHelper(instance);

    hw_init();

    setupGui();


    sntp_set_time_sync_notification_cb(time_available);
    WiFi.mode(WIFI_STA);
    WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.setAutoReconnect(false);

    Serial.println("Start done. run main loop");
}

#ifdef USING_ST25R3916
extern void loopNFCReader();
#endif

void loop()
{
    instanceLockTake();
    instance.loop();
#if defined(USING_ST25R3916)
#ifdef USING_ST25R3916
    loopNFCReader();
#endif
#endif
    lv_timer_handler();
    instanceLockGive();
    delay(5);
}

#endif