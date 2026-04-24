/**
 * @file      wireless.cpp
 * @brief     WiFi + BLE + BLE keyboard.
 */
#include "wireless.h"
#include "internal.h"
#include "board_config.h"
#include "system.h"
#include "storage.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <FFat.h>
#include <Preferences.h>
#define CONFIG_BLE_KEYBOARD
#if defined(USING_BLE_KEYBOARD)
#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEConnInfo.h>
static BleKeyboard bleKeyboard;

// iOS is strict about Apple's Accessory Design Guidelines for BLE HID. If the
// connection interval / latency / supervision timeout drift outside the
// recommended window, iPhone silently tears the link down after a few minutes.
// These values sit inside the accepted band: 30–50 ms interval, no slave
// latency, 6.72 s supervision timeout. The second-stage task also emits an
// empty HID report every 25 s so iOS sees the peripheral as "live" and does
// not park the connection during idle periods.
static TaskHandle_t ble_kb_keepalive_handle = nullptr;

static void ble_kb_apply_ios_conn_params()
{
    NimBLEServer *server = NimBLEDevice::getServer();
    if (!server) return;
    uint8_t n = server->getConnectedCount();
    for (uint8_t i = 0; i < n; ++i) {
        NimBLEConnInfo info = server->getPeerInfo(i);
        // 0x18=24 (30 ms), 0x28=40 (50 ms), latency 0, timeout 0x2A0=672 (6.72 s).
        server->updateConnParams(info.getConnHandle(), 0x18, 0x28, 0, 0x2A0);
    }
}

static void ble_kb_keepalive_task(void *)
{
    bool was_connected = false;
    uint32_t last_ka_ms = 0;
    for (;;) {
        bool now_connected = bleKeyboard.isConnected();
        if (now_connected && !was_connected) {
            // Give the iPhone a moment to finish service discovery / pairing
            // before we push a parameter update.
            vTaskDelay(pdMS_TO_TICKS(1500));
            ble_kb_apply_ios_conn_params();
            last_ka_ms = millis();
        }
        if (now_connected && (millis() - last_ka_ms) >= 25000) {
            bleKeyboard.releaseAll();
            last_ka_ms = millis();
        }
        was_connected = now_connected;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

#define WIFI_PREFS_NS     "wifi_cred"
#define WIFI_PREFS_COUNT  "count"
#define WIFI_MAX_SAVED    16

// NVS key names are capped at 15 chars; "s0".."s15" / "p0".."p15" stay well
// under that, so max-saved could be raised later if needed.
static void wifi_key_ssid(int i, char out[8]) { snprintf(out, 8, "s%d", i); }
static void wifi_key_pass(int i, char out[8]) { snprintf(out, 8, "p%d", i); }

static int wifi_saved_count_ro(Preferences &p)
{
    int n = p.getUChar(WIFI_PREFS_COUNT, 0);
    if (n > WIFI_MAX_SAVED) n = WIFI_MAX_SAVED;
    return n;
}

static bool wifi_load_at(int i, std::string &ssid, std::string &password)
{
    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, true)) return false;
    char sk[8], pk[8];
    wifi_key_ssid(i, sk);
    wifi_key_pass(i, pk);
    String s = p.getString(sk, "");
    String pw = p.getString(pk, "");
    p.end();
    if (s.length() == 0) return false;
    ssid = s.c_str();
    password = pw.c_str();
    return true;
}

// Most recent saved credential (index 0). Returns false if nothing is saved.
static bool wifi_load_saved(std::string &ssid, std::string &password)
{
    return wifi_load_at(0, ssid, password);
}
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
        hw_wifi_reconnect_saved();
    } else {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
#endif
}

void hw_wifi_reconnect_saved()
{
#ifdef ARDUINO
    if (WiFi.isConnected()) return;
    std::string s, pw;
    if (wifi_load_saved(s, pw)) {
        WiFi.begin(s.c_str(), pw.c_str());
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
    if (WiFi.getMode() == WIFI_OFF) {
        WiFi.mode(WIFI_STA);
    }
    WiFi.disconnect(false, false);
    WiFi.begin(ssid, password);
    // Saving is deferred to hw_wifi_add_saved(), called by the UI only after
    // association succeeds — that way bad passwords don't pollute favorites.
#endif
}

bool hw_get_wifi_connected()
{
#ifdef ARDUINO
    return WiFi.isConnected();
#endif
    return false;
}

void hw_set_wifi_disconnect()
{
#ifdef ARDUINO
    WiFi.disconnect(false, true);
#endif
}

bool hw_wifi_has_saved()
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, true)) return false;
    int n = wifi_saved_count_ro(p);
    p.end();
    return n > 0;
#else
    return false;
#endif
}

bool hw_wifi_get_saved_ssid(std::string &ssid)
{
#ifdef ARDUINO
    std::string pw;
    return wifi_load_saved(ssid, pw);
#else
    (void)ssid;
    return false;
#endif
}

bool hw_wifi_get_saved_password(const std::string &ssid, std::string &password)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, true)) return false;
    int n = wifi_saved_count_ro(p);
    for (int i = 0; i < n; ++i) {
        char sk[8], pk[8];
        wifi_key_ssid(i, sk);
        wifi_key_pass(i, pk);
        String s = p.getString(sk, "");
        if (s.length() == 0) continue;
        if (ssid == s.c_str()) {
            String pw = p.getString(pk, "");
            password = pw.c_str();
            p.end();
            return true;
        }
    }
    p.end();
    return false;
#else
    (void)ssid; (void)password;
    return false;
#endif
}

void hw_wifi_get_saved_list(std::vector<std::string> &ssids)
{
    ssids.clear();
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, true)) return;
    int n = wifi_saved_count_ro(p);
    for (int i = 0; i < n; ++i) {
        char sk[8];
        wifi_key_ssid(i, sk);
        String s = p.getString(sk, "");
        if (s.length() > 0) ssids.emplace_back(s.c_str());
    }
    p.end();
#endif
}

void hw_wifi_add_saved(const std::string &ssid, const std::string &password)
{
#ifdef ARDUINO
    if (ssid.empty()) return;

    // Read the existing list, dedup the incoming SSID, then prepend it.
    // Keeps most-recent at index 0 so hw_wifi_get_saved_ssid() stays useful
    // for auto-reconnect on boot.
    std::vector<std::pair<std::string, std::string>> kept;
    kept.reserve(WIFI_MAX_SAVED);
    kept.emplace_back(ssid, password);

    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, false)) return;
    int n = wifi_saved_count_ro(p);
    for (int i = 0; i < n && (int)kept.size() < WIFI_MAX_SAVED; ++i) {
        char sk[8], pk[8];
        wifi_key_ssid(i, sk);
        wifi_key_pass(i, pk);
        String s = p.getString(sk, "");
        if (s.length() == 0) continue;
        if (ssid == s.c_str()) continue;  // dedup — we just prepended it
        String pw = p.getString(pk, "");
        kept.emplace_back(s.c_str(), pw.c_str());
    }

    for (size_t i = 0; i < kept.size(); ++i) {
        char sk[8], pk[8];
        wifi_key_ssid((int)i, sk);
        wifi_key_pass((int)i, pk);
        p.putString(sk, kept[i].first.c_str());
        p.putString(pk, kept[i].second.c_str());
    }
    // Clear any trailing slots that used to be populated.
    for (size_t i = kept.size(); i < (size_t)n + 1 && i < WIFI_MAX_SAVED; ++i) {
        char sk[8], pk[8];
        wifi_key_ssid((int)i, sk);
        wifi_key_pass((int)i, pk);
        p.remove(sk);
        p.remove(pk);
    }
    p.putUChar(WIFI_PREFS_COUNT, (uint8_t)kept.size());
    p.end();
#else
    (void)ssid; (void)password;
#endif
}

void hw_wifi_forget()
{
#ifdef ARDUINO
    Preferences p;
    if (p.begin(WIFI_PREFS_NS, false)) {
        p.clear();
        p.end();
    }
    WiFi.disconnect(false, true);
#endif
}

// --- HTTP --------------------------------------------------------------

#ifdef ARDUINO
namespace {
struct HttpClients {
    WiFiClientSecure secure;
    WiFiClient plain;
    HTTPClient http;
};

// Follows redirects; returns true with `http` begin()'d on the final 2xx.
static bool http_open(HttpClients &c, const char *url, std::string *error)
{
    if (!WiFi.isConnected()) {
        if (error) *error = "WiFi not connected.";
        return false;
    }
    c.secure.setInsecure();  // GitHub Pages — we don't ship a root CA bundle.
    c.http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    c.http.setTimeout(3000);
    c.http.setConnectTimeout(3000);
    c.http.setUserAgent("LilyGoLib/1.0");

    bool ok;
    String u = url;
    if (u.startsWith("https://")) {
        ok = c.http.begin(c.secure, u);
    } else {
        ok = c.http.begin(c.plain, u);
    }
    if (!ok) {
        if (error) *error = "begin() failed.";
        return false;
    }
    int code = c.http.GET();
    if (code < 200 || code >= 300) {
        if (error) {
            char buf[64];
            snprintf(buf, sizeof(buf), "HTTP %d", code);
            *error = buf;
        }
        c.http.end();
        return false;
    }
    return true;
}
} // namespace
#endif

bool hw_ping_internet(const char *host, uint16_t port, uint32_t timeout_ms,
                      uint32_t *elapsed_ms, std::string *error)
{
#ifdef ARDUINO
    if (!WiFi.isConnected()) {
        if (error) *error = "WiFi not connected.";
        return false;
    }
    WiFiClient client;
    client.setTimeout(timeout_ms / 1000 ? timeout_ms / 1000 : 1);
    uint32_t t0 = millis();
    int rc = client.connect(host, port, timeout_ms);
    uint32_t dt = millis() - t0;
    if (!rc) {
        client.stop();
        if (error) *error = "Unreachable.";
        return false;
    }
    client.stop();
    if (elapsed_ms) *elapsed_ms = dt;
    return true;
#else
    (void)host; (void)port; (void)timeout_ms;
    if (elapsed_ms) *elapsed_ms = 0;
    if (error) *error = "Not supported on emulator.";
    return false;
#endif
}

bool hw_http_get_string(const char *url, std::string &out, std::string *error)
{
#ifdef ARDUINO
    HttpClients c;
    if (!http_open(c, url, error)) return false;
    String body = c.http.getString();
    out.append(body.c_str(), body.length());
    c.http.end();
    return true;
#else
    (void)url; (void)out; (void)error;
    return false;
#endif
}

bool hw_http_request(const char *url,
                     const char *method,
                     const char *body,
                     size_t body_len,
                     const char *content_type,
                     const char *auth_header,
                     std::string &out,
                     int *status_code,
                     std::string *error)
{
#ifdef ARDUINO
    if (!WiFi.isConnected()) {
        if (error) *error = "WiFi not connected.";
        if (status_code) *status_code = 0;
        return false;
    }
    HttpClients c;
    c.secure.setInsecure();
    c.http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    c.http.setTimeout(3000);
    c.http.setConnectTimeout(3000);
    c.http.setUserAgent("LilyGoLib/1.0");

    String u = url;
    bool begun = u.startsWith("https://") ? c.http.begin(c.secure, u)
                                           : c.http.begin(c.plain, u);
    if (!begun) {
        if (error) *error = "begin() failed.";
        if (status_code) *status_code = 0;
        return false;
    }
    if (auth_header && *auth_header) {
        c.http.addHeader("Authorization", auth_header);
    }
    if (content_type && *content_type && body) {
        c.http.addHeader("Content-Type", content_type);
    }

    int code;
    const char *m = method && *method ? method : "GET";
    if (body) {
        code = c.http.sendRequest(m, (uint8_t *)body, body_len);
    } else {
        code = c.http.sendRequest(m);
    }
    if (status_code) *status_code = code;

    // Always capture the body so callers can read error payloads too.
    String rbody = c.http.getString();
    out.append(rbody.c_str(), rbody.length());
    c.http.end();

    if (code < 200 || code >= 300) {
        if (error) {
            char buf[48];
            snprintf(buf, sizeof(buf), "HTTP %d", code);
            *error = buf;
        }
        return false;
    }
    return true;
#else
    (void)url; (void)method; (void)body; (void)body_len;
    (void)content_type; (void)auth_header; (void)out;
    if (status_code) *status_code = 0;
    if (error) *error = "Not supported on emulator.";
    return false;
#endif
}

bool hw_http_download_to_file(const char *url, const char *abs_path,
                              size_t *bytes_written,
                              bool (*progress_cb)(size_t, size_t),
                              std::string *error)
{
#ifdef ARDUINO
    HttpClients c;
    if (!http_open(c, url, error)) return false;

    int total = c.http.getSize();  // -1 if unknown
    WiFiClient *stream = c.http.getStreamPtr();
    if (!stream) {
        if (error) *error = "No stream.";
        c.http.end();
        return false;
    }

    String path = (abs_path[0] == '/') ? String(abs_path) : ("/" + String(abs_path));
    bool is_sd = (HW_SD_ONLINE & hw_get_device_online());

    // Ensure the parent directory exists (e.g. "/news" on first download).
    int last_slash = path.lastIndexOf('/');
    if (last_slash > 0) {
        String dir = path.substring(0, last_slash);
        if (is_sd) {
            instance.lockSPI();
            if (!SD.exists(dir)) SD.mkdir(dir);
            instance.unlockSPI();
        }
        if (!FFat.exists(dir)) FFat.mkdir(dir);
    }

    File f;
    bool lock = false;
    if (is_sd) {
        instance.lockSPI();
        f = SD.open(path, "w");
        if (f) {
            lock = true;
        } else {
            instance.unlockSPI();
        }
    }
    if (!f) f = FFat.open(path, "w");
    if (!f) {
        if (error) *error = "Cannot open destination file.";
        c.http.end();
        return false;
    }

    uint8_t buf[1024];
    size_t written = 0;
    bool aborted = false;

    while (c.http.connected() && (total < 0 || (int)written < total)) {
        size_t avail = stream->available();
        if (avail) {
            size_t to_read = avail < sizeof(buf) ? avail : sizeof(buf);
            int n = stream->readBytes(buf, to_read);
            if (n <= 0) break;
            if (f.write(buf, n) != (size_t)n) {
                if (error) *error = "Write failed (full?).";
                aborted = true;
                break;
            }
            written += n;
            if (progress_cb && !progress_cb(written, total > 0 ? (size_t)total : 0)) {
                aborted = true;
                break;
            }
        } else {
            delay(5);
        }
    }

    f.close();
    if (lock) instance.unlockSPI();
    c.http.end();

    if (aborted) {
        hw_delete_file(abs_path);
        return false;
    }
    if (bytes_written) *bytes_written = written;
    return true;
#else
    (void)url; (void)abs_path; (void)bytes_written;
    (void)progress_cb; (void)error;
    return false;
#endif
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
    // BleKeyboard's begin() calls setSecurityAuth(false,false,false), which
    // leaves bonding off — iOS then re-prompts "Allow" on every reconnect.
    // Re-enable bonding with Just Works pairing so the link key persists.
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    if (!ble_kb_keepalive_handle) {
        xTaskCreate(ble_kb_keepalive_task, "ble_kb_ka", 3072, nullptr,
                    tskIDLE_PRIORITY + 1, &ble_kb_keepalive_handle);
    }
#endif
#endif
}

void hw_set_ble_kb_disable()
{
#if defined(ARDUINO) && defined(USING_BLE_KEYBOARD)
    if (ble_kb_keepalive_handle) {
        vTaskDelete(ble_kb_keepalive_handle);
        ble_kb_keepalive_handle = nullptr;
    }
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
