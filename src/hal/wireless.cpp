/**
 * @file      wireless.cpp
 * @brief     WiFi + BLE + BLE keyboard.
 */
#include "wireless.h"
#include "internal.h"
#include "board_config.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#include <WiFi.h>
#define CONFIG_BLE_KEYBOARD
#if defined(USING_BLE_KEYBOARD)
#include <BleKeyboard.h>
static BleKeyboard bleKeyboard;
#endif
#else
#include <cstring>
#endif

// --- WiFi enable / state ----------------------------------------------

bool hw_get_wifi_enable() { return user_setting.wifi_enable; }
void hw_set_wifi_enable(bool en) {
    user_setting.wifi_enable = en;
#ifdef ARDUINO
    if (en) {
        WiFi.mode(WIFI_STA);
    } else {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
#endif
}

void hw_get_wifi_ssid(std::string &param)
{
#ifdef ARDUINO
    param = WiFi.isConnected() ?  WiFi.SSID().c_str() : "N.A";
#else
    param = "NO CONFIG";
#endif
}

wl_status_t hw_get_wifi_status()
{
#ifdef ARDUINO
    return WiFi.status();
#else
    return WL_NO_SSID_AVAIL;
#endif
}

void hw_get_ip_address(std::string &param)
{
#ifdef ARDUINO
    if (WiFi.isConnected()) {
        param = WiFi.localIP().toString().c_str();
        return;
    }
#endif
    param = "N.A";
}

int16_t hw_get_wifi_rssi()
{
#ifdef ARDUINO
    if (WiFi.isConnected()) {
        return (WiFi.RSSI());
    }
#endif
    return -99;
}

int16_t hw_set_wifi_scan()
{
#ifdef ARDUINO
    printf("hw_set_wifi_scan\n");
    return  WiFi.scanNetworks(true);
#endif
    return 0;
}

bool hw_get_wifi_scanning()
{
#ifdef ARDUINO
    return WiFi.getStatusBits() & WIFI_SCANNING_BIT ;
#endif
    return false;
}

void hw_get_wifi_scan_result(std::vector<wifi_scan_params_t> &list)
{
    list.clear();
#ifdef ARDUINO
    int16_t nums = WiFi.scanComplete();
    if (nums < 0) {
        printf("Nothing network found. return code : %d\n", nums);
        return;
    } else {
        printf("find %d network\n", nums);
    }
    // uint8_t networkItem, String &ssid, uint8_t &encryptionType, int32_t &RSSI, uint8_t *&BSSID, int32_t &channel
    wifi_scan_params_t param;
    for (int i = 0; i < nums; ++i) {
        String ssid;
        uint8_t encryptionType;
        int32_t rssi;
        uint8_t *BSSID;
        int32_t channel;
        WiFi.getNetworkInfo(i, ssid, encryptionType, rssi, BSSID, channel);
        printf("SSID:%s RSSI:%d\n", ssid.c_str(), rssi);
        param.authmode = encryptionType;
        param.ssid = ssid.c_str();
        param.rssi = rssi;
        param.channel = channel;
        memcpy(param.bssid, BSSID, 6);
        list.push_back(param);
    }
#else
    wifi_scan_params_t param;
    param.authmode = 1;
    param.ssid = "LilyGo-AABB0";
    param.rssi = -10;
    param.channel = 0;
    list.push_back(param);
#endif
}

void hw_set_wifi_connect(wifi_conn_params_t &params)
{
    printf("hw_set_wifi_connect:ssid:<%s> password <%s>\n", params.ssid.c_str(), params.password.c_str());
#ifdef ARDUINO
    String ssid = params.ssid.c_str();
    String password = params.password.c_str();
    Serial.print("SSID :"); Serial.println(ssid);
    Serial.print("PWD :"); Serial.println(password);
    WiFi.begin(ssid, password);
#endif
}

bool hw_get_wifi_connected()
{
#ifdef ARDUINO
    return WiFi.isConnected();
#endif
    return false;
}

// --- BLE (UART) -------------------------------------------------------

bool hw_get_bt_enable() { return user_setting.bt_enable; }
void hw_set_bt_enable(bool en) {
    user_setting.bt_enable = en;
    if (en) {
        hw_set_ble_kb_enable();
    } else {
        hw_set_ble_kb_disable();
    }
}

void hw_enable_ble(const char *devName)
{
#if  defined(ARDUINO) && defined(USING_UART_BLE)
#endif
}

void hw_deinit_ble()
{
#if  defined(ARDUINO) && defined(USING_UART_BLE)

#endif
}

void hw_disable_ble()
{
#if  defined(ARDUINO) && defined(USING_UART_BLE)

#endif
}

size_t hw_get_ble_message(char *buffer, size_t buffer_size)
{
#if  defined(ARDUINO) && defined(USING_UART_BLE)
#endif
    return 0;
}

// --- BLE keyboard ------------------------------------------------------

const char *hw_get_ble_kb_name()
{
    return "lilygo-pager";
}

void hw_set_ble_kb_enable()
{
#if defined(ARDUINO) && defined(USING_BLE_KEYBOARD)
#ifdef CONFIG_BLE_KEYBOARD
    bleKeyboard.setName("lilygo-pager");
    bleKeyboard.begin();
#endif
#endif
}

void hw_set_ble_kb_disable()
{
#if defined(ARDUINO) && defined(USING_BLE_KEYBOARD)
    bleKeyboard.end();
    log_d("Disable ble devices");
#endif
}

void hw_set_ble_kb_char(const char *c)
{
#if defined(ARDUINO) && defined(USING_BLE_KEYBOARD)
#ifdef CONFIG_BLE_KEYBOARD
    if (bleKeyboard.isConnected()) {
        bleKeyboard.print(c);
    }
#endif
#endif
}

void hw_set_ble_kb_key(uint8_t key)
{
#if defined(ARDUINO) && defined(USING_BLE_KEYBOARD)
#ifdef CONFIG_BLE_KEYBOARD
    if (bleKeyboard.isConnected()) {
        bleKeyboard.press(key);
    }
#endif
#endif
}

void hw_set_ble_kb_release()
{
#if defined(ARDUINO) && defined(USING_BLE_KEYBOARD)
#ifdef CONFIG_BLE_KEYBOARD
    if (bleKeyboard.isConnected()) {
        bleKeyboard.releaseAll();
    }
#endif
#endif
}

bool hw_get_ble_kb_connected()
{
#if defined(ARDUINO) && defined(USING_BLE_KEYBOARD)
#ifdef CONFIG_BLE_KEYBOARD
    if (bleKeyboard.isConnected()) {
        return true;
    }
#endif
#endif
    return false;
}

void hw_set_ble_key(media_key_value_t key)
{
#if defined(ARDUINO) && defined(USING_BLE_KEYBOARD)
#ifdef CONFIG_BLE_KEYBOARD
    if (bleKeyboard.isConnected()) {
        switch (key) {
        case MEDIA_VOLUME_UP:
            bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
            break;
        case MEDIA_VOLUME_DOWN:
            bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
            break;
        case MEDIA_PLAY_PAUSE:
            bleKeyboard.write(KEY_MEDIA_PLAY_PAUSE);
            break;
        case MEDIA_NEXT:
            bleKeyboard.write(KEY_MEDIA_NEXT_TRACK);
            break;
        case MEDIA_PREVIOUS:
            bleKeyboard.write(KEY_MEDIA_PREVIOUS_TRACK);
            break;
        default: return;
        }

    }
#endif
#endif
}
