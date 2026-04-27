/**
 * @file      hub.cpp
 * @brief     Local hub config storage. See hub.h.
 */
#include "hub.h"
#include "wireless.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#endif

namespace hal {

namespace {
constexpr const char *NS = "hub";
constexpr const char *KEY_URL = "url";
constexpr const char *KEY_ENABLED = "enabled";

// Legacy slot — hub URL used to live under the "weather" namespace before this
// module existed. See hub.h for the migration contract.
constexpr const char *LEGACY_NS = "weather";
constexpr const char *LEGACY_KEY = "hub_url";

std::string trim_url(const std::string &in)
{
    std::string s = in;
    while (!s.empty() && (s.back() == '/' || s.back() == ' ' || s.back() == '\r' ||
                          s.back() == '\n' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a);
}

#ifdef ARDUINO
// One-shot legacy migration: if the new slot is empty but the old one has a
// value, copy it across, force the toggle ON (preserving prior behavior —
// having set the URL meant the user wanted hub-first), and delete the legacy
// key. Safe to call repeatedly: once the legacy slot is gone, this is a no-op.
void maybe_migrate_legacy()
{
    Preferences neu;
    if (!neu.begin(NS, true)) return;
    bool have_new = neu.isKey(KEY_URL);
    neu.end();
    if (have_new) return;

    Preferences old;
    if (!old.begin(LEGACY_NS, false)) return;
    String legacy = old.getString(LEGACY_KEY, "");
    if (legacy.length() == 0) {
        old.end();
        return;
    }
    old.remove(LEGACY_KEY);
    old.end();

    Preferences w;
    if (!w.begin(NS, false)) return;
    w.putString(KEY_URL, legacy);
    w.putBool(KEY_ENABLED, true);
    w.end();
}
#endif

} // namespace

std::string hub_get_url_raw()
{
#ifdef ARDUINO
    maybe_migrate_legacy();
    Preferences p;
    if (!p.begin(NS, true)) return "";
    String h = p.getString(KEY_URL, "");
    p.end();
    return trim_url(h.c_str());
#else
    return "";
#endif
}

bool hub_get_enabled_pref()
{
#ifdef ARDUINO
    maybe_migrate_legacy();
    Preferences p;
    if (!p.begin(NS, true)) return false;
    bool e = p.getBool(KEY_ENABLED, false);
    p.end();
    return e;
#else
    return false;
#endif
}

bool hub_is_enabled()
{
    if (!hub_get_enabled_pref()) return false;
    return !hub_get_url_raw().empty();
}

std::string hub_get_url()
{
    if (!hub_is_enabled()) return "";
    return hub_get_url_raw();
}

void hub_set_enabled(bool enabled)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NS, false)) return;
    p.putBool(KEY_ENABLED, enabled);
    p.end();
#else
    (void)enabled;
#endif
}

void hub_set_url(const char *url)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NS, false)) return;
    if (url && *url) {
        p.putString(KEY_URL, url);
    } else {
        p.remove(KEY_URL);
    }
    p.end();
#else
    (void)url;
#endif
}

bool hub_is_reachable(uint32_t timeout_ms)
{
#ifdef ARDUINO
    if (!hub_is_enabled()) return false;
    if (!hw_get_wifi_connected()) return false;

    std::string url = hub_get_url();

    // Parse `http[s]://host[:port][/...]`. We only support http here — the
    // hub is a LAN service. Defaults match the URL scheme: http=80.
    size_t scheme_end = url.find("://");
    uint16_t port = 80;
    std::string rest;
    if (scheme_end != std::string::npos) {
        std::string scheme = url.substr(0, scheme_end);
        if (scheme == "https") port = 443;
        rest = url.substr(scheme_end + 3);
    } else {
        rest = url;
    }
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    if (hostport.empty()) return false;

    std::string host = hostport;
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        long p = strtol(hostport.c_str() + colon + 1, nullptr, 10);
        if (p > 0 && p < 65536) port = (uint16_t)p;
    }
    if (host.empty()) return false;

    return hw_ping_internet(host.c_str(), port, timeout_ms, nullptr, nullptr);
#else
    (void)timeout_ms;
    return false;
#endif
}

} // namespace hal
