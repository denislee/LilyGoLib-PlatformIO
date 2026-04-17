/**
 * @file      wireless.h
 * @brief     WiFi and BLE (including BLE keyboard).
 */
#pragma once

#include "types.h"

// --- WiFi ---
void hw_get_wifi_ssid(std::string &param);
wl_status_t hw_get_wifi_status();
void hw_get_ip_address(std::string &param);
int16_t hw_get_wifi_rssi();

int16_t hw_set_wifi_scan();
bool hw_get_wifi_scanning();
void hw_get_wifi_scan_result(std::vector<wifi_scan_params_t> &list);
void hw_set_wifi_connect(wifi_conn_params_t &params);
bool hw_get_wifi_connected();

bool hw_get_wifi_enable();
void hw_set_wifi_enable(bool en);

// --- BLE ---
bool hw_get_bt_enable();
void hw_set_bt_enable(bool en);

void hw_enable_ble(const char *devName);
void hw_disable_ble();
size_t hw_get_ble_message(char *buffer, size_t buffer_size);
void hw_deinit_ble();

// --- BLE keyboard ---
const char *hw_get_ble_kb_name();
void hw_set_ble_kb_enable();
void hw_set_ble_kb_disable();
void hw_set_ble_kb_char(const char *c);
void hw_set_ble_kb_key(uint8_t key);
void hw_set_ble_kb_release();
bool hw_get_ble_kb_connected();
void hw_set_ble_key(media_key_value_t key);
