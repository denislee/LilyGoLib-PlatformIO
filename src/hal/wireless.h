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
void hw_set_wifi_disconnect();

bool hw_get_wifi_enable();
void hw_set_wifi_enable(bool en);

// --- WiFi saved credentials (persistent across reboots) ---
// Favorites list: every network the user has successfully connected to is
// stored; the most recent lands at index 0.
bool hw_wifi_has_saved();
bool hw_wifi_get_saved_ssid(std::string &ssid);  // most recent
bool hw_wifi_get_saved_password(const std::string &ssid, std::string &password);
void hw_wifi_get_saved_list(std::vector<std::string> &ssids);
void hw_wifi_add_saved(const std::string &ssid, const std::string &password);
void hw_wifi_forget();  // drop every saved network

// --- HTTP (blocking; require an active WiFi connection) ---
// GET the URL and append the body to `out`. Returns true on 2xx response.
// `error` (optional) receives a short diagnostic on failure.
bool hw_http_get_string(const char *url, std::string &out, std::string *error = nullptr);

// Flexible HTTP call. Set `method` to "GET", "POST", etc. A `body` != nullptr
// is sent verbatim with `content_type`. `auth_header` (e.g. "Bearer abc123")
// is added as the Authorization header when non-null.
//
// On completion `status_code` (optional) receives the HTTP status regardless
// of success. The function returns true only on 2xx responses; non-2xx sets
// `error` to "HTTP <code>" and returns false but still populates `out` so
// callers can inspect error bodies if they care.
bool hw_http_request(const char *url,
                     const char *method,
                     const char *body,
                     size_t body_len,
                     const char *content_type,
                     const char *auth_header,
                     std::string &out,
                     int *status_code = nullptr,
                     std::string *error = nullptr);

// Quick internet reachability test. Opens a TCP connection to `host:port` with
// `timeout_ms`, returns true on success. On success `elapsed_ms` (optional)
// receives the round-trip time to connect. On failure `error` (optional) gets
// a short diagnostic. Defaults target Cloudflare DNS (1.1.1.1:53) since it is
// always-up, firewall-friendly, and does not require ICMP.
bool hw_ping_internet(const char *host = "1.1.1.1",
                      uint16_t port = 53,
                      uint32_t timeout_ms = 3000,
                      uint32_t *elapsed_ms = nullptr,
                      std::string *error = nullptr);

// GET the URL and stream the body to `abs_path` (absolute path on SD or FFat).
// On success returns true and `bytes_written` (optional) gets the byte count.
// `progress_cb` is invoked periodically with (downloaded, total_or_0). Return
// false from the callback to abort — the partial file is then removed.
bool hw_http_download_to_file(const char *url, const char *abs_path,
                              size_t *bytes_written = nullptr,
                              bool (*progress_cb)(size_t, size_t) = nullptr,
                              std::string *error = nullptr);

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
