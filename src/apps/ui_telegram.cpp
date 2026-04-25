/**
 * @file      ui_telegram.cpp
 * @brief     Telegram client via tg-bridge (HTTPS + bearer).
 *
 * Single-account polling client. The bridge (a Go service the user runs on
 * a Raspberry Pi or VPS) holds the MTProto session; this app only speaks
 * a narrow JSON/bearer API to it. No WebSocket client on-device — we poll
 * /v1/chats and /v1/chats/{id}/messages on an LVGL timer while the
 * relevant view is open.
 *
 * Bridge URL and bearer token are managed from the Settings app
 * (Settings → Telegram). The cfg helpers at the bottom of this file are
 * what the settings subpage calls into.
 *
 * Text rendering: LVGL's UTF-8 path looks up glyphs by codepoint and skips
 * missing ones, so non-ASCII characters survive if the selected font has
 * the glyph. The Inter font (idx 4) covers Latin-1 (á, é, ã, ç, …) — it's
 * the default for Telegram so Portuguese messages render out of the box.
 */
#include "../ui_define.h"
#include "../hal/wireless.h"
#include "../hal/system.h"
#include "../hal/notes_crypto.h"
#include "../hal/secrets.h"
#include "../core/app.h"
#include "../core/app_manager.h"
#include "../core/notify.h"
#include "../core/system.h"
#include "../core/input_focus.h"
#include "app_registry.h"
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Preferences.h>
#include "../core/scoped_lock.h"
extern "C" {
#include "cJSON.h"
}
#endif

namespace {

#define TG_PREFS_NS       "tgbridge"
#define TG_LIST_POLL_MS   10000
#define TG_CHAT_POLL_MS   5000
#define TG_CHAT_LIMIT     30
#define TG_MSG_LIMIT      20

// --- persisted config ------------------------------------------------------

static std::string load_pref(const char *key)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(TG_PREFS_NS, true)) return "";
    String v = p.getString(key, "");
    p.end();
    return std::string(v.c_str());
#else
    (void)key;
    return "";
#endif
}

static void save_pref(const char *key, const char *value)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(TG_PREFS_NS, false)) return;
    if (value && *value) p.putString(key, value);
    else p.remove(key);
    p.end();
#else
    (void)key; (void)value;
#endif
}

static bool load_bool_pref(const char *key, bool dflt)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(TG_PREFS_NS, true)) return dflt;
    bool v = p.getBool(key, dflt);
    p.end();
    return v;
#else
    (void)key;
    return dflt;
#endif
}

static void save_bool_pref(const char *key, bool value)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(TG_PREFS_NS, false)) return;
    p.putBool(key, value);
    p.end();
#else
    (void)key; (void)value;
#endif
}

// The bearer lives in NVS as `token_enc` (AES-256-CBC + PBKDF2, OpenSSL-enc
// format) produced by notes_crypto. We piggyback on the notes passphrase —
// one unlock per session covers every secret slot device-wide. Persistence
// delegated to hal/secrets; legacy plaintext `token` slot is wiped at boot.
static bool token_is_encrypted() {
    return hal::secret_exists(TG_PREFS_NS, "token_enc");
}

static std::string load_token_plain() {
    return hal::secret_load(TG_PREFS_NS, "token_enc");
}

static bool save_token(const char *value, std::string *err) {
    bool ok = hal::secret_store(TG_PREFS_NS, "token_enc", value, err);
    // Clearing or rewriting the encrypted slot also nukes any legacy
    // plaintext sibling so the two never coexist.
    if (ok) hal::secret_purge_legacy(TG_PREFS_NS, "token");
    return ok;
}

static void purge_legacy_plaintext_token() {
    hal::secret_purge_legacy(TG_PREFS_NS, "token");
}

static void scrub_string(std::string &s) { hal::secret_scrub(s); }

// --- state ----------------------------------------------------------------

struct Chat {
    long long id;
    std::string title;
    int unread;
};

struct Message {
    int id;
    std::string from;
    std::string text;
    bool out;
    // Bridge sends a "media" object for non-text messages (photo, video,
    // voice, document, sticker, other). We don't download the payload yet —
    // just surface a placeholder line in the bubble so the chat doesn't look
    // empty when an image arrives.
    std::string media_type;
    int media_w;
    int media_h;
};

static lv_obj_t *s_root = nullptr;
static lv_timer_t *s_timer = nullptr;
static std::string s_base_url;
static std::string s_auth_header;     // "Bearer <token>" or empty

enum View { V_NONE, V_LIST, V_CHAT, V_NOT_CONFIGURED };
static View s_view = V_NONE;

static long long s_current_chat_id = 0;
static std::string s_current_chat_title;
// Highest message id we've asked the bridge to mark as read in the current
// chat view. Reset to 0 whenever we open a new chat so the first fetch
// always fires the mark-read POST.
static int s_last_marked_msg_id = 0;

static lv_obj_t *s_list_holder = nullptr;
static lv_obj_t *s_msgs_holder = nullptr;
// True while the user has clicked into the history to scroll it. Polling is
// paused in this state because a refetch re-renders and pins scroll to the
// bottom, yanking the view back down while the user is trying to scroll up.
static bool s_msgs_scroll_mode = false;
static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_input_ta = nullptr;
static int32_t   s_input_expanded_h = 0;

static std::vector<Chat> s_chats;
static std::vector<Message> s_msgs;

// Favorite chat IDs. The chat list in the Telegram app filters to this set;
// the full list is only visible from Settings → Telegram → Favorites.
static std::set<long long> s_favorites;
static bool s_favorites_loaded = false;

// Set by onStart / on_unlocked so the next entry into the chat list auto-
// jumps into the single favorite when there's exactly one. Cleared after
// one attempt so pressing Back from the chat view lands on the list.
static bool s_auto_enter_single_pending = false;

// Unread-count state shared with the home-menu badge. Updated by the in-app
// poll when the Telegram view is open, and by the background poll otherwise
// (see tg_bg_timer below).
static int s_unread_total = 0;
static lv_timer_t *s_bg_timer = nullptr;

// Baseline for the background notifier. -1 means "not initialized yet": the
// first successful bg poll seeds it so we don't buzz on boot for messages
// that arrived while the device was off. Subsequent polls fire enabled
// notifiers only when `sum > s_last_notified_unread`.
static int s_last_notified_unread = -1;

#ifdef ARDUINO
static TaskHandle_t s_bg_task = nullptr;
#endif

// --- forward decls --------------------------------------------------------

static void show_chat_list();
static void show_chat(long long id, const char *title);
static void show_not_configured();

// --- helpers --------------------------------------------------------------

// Favorites are persisted as a comma-separated list of numeric chat IDs
// (matches the bridge's long-long id space). Parsed and re-serialized
// whenever the user toggles a star in settings.
static void load_favorites()
{
    s_favorites.clear();
    s_favorites_loaded = true;
    std::string raw = load_pref("favs");
    if (raw.empty()) return;
    size_t i = 0;
    while (i < raw.size()) {
        size_t j = raw.find(',', i);
        std::string tok = raw.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!tok.empty()) {
            long long v = strtoll(tok.c_str(), nullptr, 10);
            if (v != 0) s_favorites.insert(v);
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
}

static void save_favorites()
{
    std::string joined;
    joined.reserve(s_favorites.size() * 12);
    for (auto it = s_favorites.begin(); it != s_favorites.end(); ++it) {
        if (!joined.empty()) joined.push_back(',');
        char b[24];
        snprintf(b, sizeof(b), "%lld", *it);
        joined += b;
    }
    save_pref("favs", joined.c_str());
}

static void reload_config()
{
    s_base_url = load_pref("url");
    std::string tok = load_token_plain();
    s_auth_header = tok.empty() ? std::string() : ("Bearer " + tok);
    if (!s_favorites_loaded) load_favorites();
}

static bool configured()
{
    return !s_base_url.empty() && !s_auth_header.empty();
}

static void set_status(const char *text, lv_color_t color)
{
    if (!s_status_label) return;
    const bool empty = !text || !*text;
    lv_label_set_text(s_status_label, text ? text : "");
    lv_obj_set_style_text_color(s_status_label, color, 0);
    if (empty) lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
}

static std::string make_url(const char *path)
{
    std::string u = s_base_url;
    if (!u.empty() && u.back() == '/') u.pop_back();
    if (path && *path != '/') u.push_back('/');
    if (path) u += path;
    return u;
}

// Drop any codepoint the Telegram font (Inter + emoji fallback) can't
// render so the label doesn't show a tofu rectangle for it. Ask LVGL
// directly: lv_font_get_glyph_dsc walks the fallback chain, so ASCII +
// Latin-1 go through Inter, curated emoji through font_emoji_*, and
// anything else — CJK, rare symbols, unsupported emoji — is elided.
//
// Also strips bare control bytes (except \n/\t) and discards malformed
// UTF-8.
static std::string ascii_safe(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    const lv_font_t *font = get_telegram_font();
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20) {
            if (c == '\n' || c == '\t') out.push_back((char)c);
            i++; continue;
        }
        size_t extra;
        uint32_t cp;
        if (c < 0x80)                { out.push_back((char)c); i++; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else { i++; continue; }
        if (i + extra >= in.size()) break;
        bool bad = false;
        for (size_t k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)in[i + k];
            if ((cc & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!bad) {
            lv_font_glyph_dsc_t dsc;
            if (font && lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
                out.append(in, i, 1 + extra);
            }
        }
        i += 1 + extra;
    }
    return out;
}

// Serialize a single {"text":"..."} JSON body with minimal escaping.
static std::string json_text_body(const char *text)
{
    std::string b;
    b.reserve(16 + (text ? strlen(text) : 0));
    b += "{\"text\":\"";
    for (const char *p = text; p && *p; p++) {
        char c = *p;
        switch (c) {
            case '"':  b += "\\\""; break;
            case '\\': b += "\\\\"; break;
            case '\n': b += "\\n";  break;
            case '\r':              break;
            case '\t': b += "\\t";  break;
            default:
                if ((unsigned char)c >= 0x20) b.push_back(c);
                break;
        }
    }
    b += "\"}";
    return b;
}

// --- HTTP -----------------------------------------------------------------

#ifdef ARDUINO
// Cached internet reachability. WiFi-associated does not imply the bridge is
// reachable (captive portals, ISP outage, DNS down), and the bridge HTTPS
// timeout is multi-second — so we probe TCP/53 to 1.1.1.1 and cache the
// verdict. Success is cached longer than failure so a recovering link is
// retried sooner without spamming the probe on every poll tick.
static const uint32_t TG_INET_OK_TTL_MS   = 30000;
static const uint32_t TG_INET_FAIL_TTL_MS = 5000;
static const uint32_t TG_INET_PING_MS     = 1500;
static uint32_t s_inet_check_ts = 0;
static bool     s_inet_ok       = false;

static bool internet_available()
{
    if (!hw_get_wifi_connected()) {
        s_inet_ok = false;
        s_inet_check_ts = lv_tick_get();
        return false;
    }
    uint32_t ttl = s_inet_ok ? TG_INET_OK_TTL_MS : TG_INET_FAIL_TTL_MS;
    if (s_inet_check_ts != 0 && lv_tick_elaps(s_inet_check_ts) < ttl) {
        return s_inet_ok;
    }
    s_inet_ok = hw_ping_internet("1.1.1.1", 53, TG_INET_PING_MS, nullptr, nullptr);
    s_inet_check_ts = lv_tick_get();
    return s_inet_ok;
}

static bool require_internet(std::string *err)
{
    if (internet_available()) return true;
    if (err) *err = hw_get_wifi_connected() ? "No internet" : "WiFi not connected";
    return false;
}

static bool tg_get(const char *path, std::string &out, std::string *err = nullptr)
{
    if (!require_internet(err)) return false;
    std::string url = make_url(path);
    int code = 0;
    return hw_http_request(url.c_str(), "GET", nullptr, 0, nullptr,
                           s_auth_header.c_str(), out, &code, err);
}

static bool tg_post_json(const char *path, const std::string &body,
                         std::string &out, std::string *err = nullptr)
{
    if (!require_internet(err)) return false;
    std::string url = make_url(path);
    int code = 0;
    return hw_http_request(url.c_str(), "POST",
                           body.c_str(), body.size(),
                           "application/json",
                           s_auth_header.c_str(), out, &code, err);
}
#endif

// --- chat list ------------------------------------------------------------

static void chat_click_cb(lv_event_t *e)
{
    auto *chat = (Chat *)lv_event_get_user_data(e);
    if (!chat) return;
    hw_feedback();
    show_chat(chat->id, chat->title.c_str());
}

static void render_chats()
{
    if (!s_list_holder) return;
    lv_obj_clean(s_list_holder);
    size_t shown = 0;
    for (size_t i = 0; i < s_chats.size(); i++) {
        Chat &c = s_chats[i];
        if (s_favorites.find(c.id) == s_favorites.end()) continue;
        std::string label = ascii_safe(c.title);
        if (c.unread > 0) {
            char b[16];
            snprintf(b, sizeof(b), " (%d)", c.unread);
            label += b;
        }
        lv_obj_t *btn = lv_list_add_button(s_list_holder, NULL, label.c_str());
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(btn, get_telegram_list_font(), 0);
        lv_obj_add_event_cb(btn, chat_click_cb, LV_EVENT_CLICKED, &c);
        shown++;
    }
    if (shown == 0) {
        lv_obj_t *hint = lv_label_create(s_list_holder);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_label_set_text(hint,
            "No favorites yet.\n"
            "Add chats in Settings " "\xC2\xBB" " Telegram " "\xC2\xBB" " Favorites.");
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_text_color(hint, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_font(hint, get_telegram_font(), 0);
        lv_obj_set_style_pad_all(hint, 6, 0);
    }
}

#ifdef ARDUINO
static bool fetch_chats()
{
    if (s_bg_task != nullptr) {
        set_status("Syncing in background...", UI_COLOR_MUTED);
        return false;
    }
    set_status("Loading...", UI_COLOR_ACCENT);
    lv_refr_now(nullptr);

    std::string body, err;
    char path[64];
    snprintf(path, sizeof(path), "/v1/chats?limit=%d", TG_CHAT_LIMIT);
    if (!tg_get(path, body, &err)) {
        set_status(err.c_str(), lv_palette_main(LV_PALETTE_RED));
        return false;
    }
    cJSON *arr = cJSON_Parse(body.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        set_status("Parse error", lv_palette_main(LV_PALETTE_RED));
        if (arr) cJSON_Delete(arr);
        return false;
    }
    s_chats.clear();
    int n = cJSON_GetArraySize(arr);
    s_chats.reserve((size_t)n);
    int unread_sum = 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        Chat c;
        cJSON *jid  = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jti  = cJSON_GetObjectItemCaseSensitive(it, "title");
        cJSON *junr = cJSON_GetObjectItemCaseSensitive(it, "unread");
        c.id     = (jid  && cJSON_IsNumber(jid))  ? (long long)jid->valuedouble : 0;
        c.title  = (jti  && cJSON_IsString(jti))  ? jti->valuestring : "(no title)";
        c.unread = (junr && cJSON_IsNumber(junr)) ? (int)junr->valuedouble : 0;
        if (c.unread > 0) unread_sum += c.unread;
        s_chats.push_back(std::move(c));
    }
    cJSON_Delete(arr);
    s_unread_total = unread_sum;
    render_chats();
    set_status("", UI_COLOR_MUTED);
    return true;
}
#else
static bool fetch_chats()
{
    set_status("Not supported on emulator.", UI_COLOR_MUTED);
    return false;
}
#endif

// --- chat view (messages + send) ------------------------------------------

static void render_msgs()
{
    if (!s_msgs_holder) return;
    lv_obj_clean(s_msgs_holder);
    // API returns newest-first; display oldest-at-top so the newest sits at
    // the bottom where the user expects.
    for (auto it = s_msgs.rbegin(); it != s_msgs.rend(); ++it) {
        // Transparent full-width wrapper whose flex alignment anchors the
        // bubble to the left (incoming) or right (own messages).
        lv_obj_t *wrap = lv_obj_create(s_msgs_holder);
        lv_obj_set_width(wrap, lv_pct(100));
        lv_obj_set_height(wrap, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wrap, 0, 0);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wrap,
            it->out ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        lv_obj_t *row = lv_obj_create(wrap);
        lv_obj_set_width(row, lv_pct(85));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, it->out ? lv_color_hex(0x3a3a3a)
                                               : lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);

        if (!it->out && !it->from.empty()) {
            lv_obj_t *from = lv_label_create(row);
            lv_label_set_text(from, ascii_safe(it->from).c_str());
            lv_obj_set_style_text_color(from, UI_COLOR_ACCENT, 0);
            lv_obj_set_style_text_font(from, get_telegram_font(), 0);
        }

        if (!it->media_type.empty()) {
            const char *label;
            if      (it->media_type == "photo")    label = "[Photo]";
            else if (it->media_type == "video")    label = "[Video]";
            else if (it->media_type == "voice")    label = "[Voice]";
            else if (it->media_type == "sticker")  label = "[Sticker]";
            else if (it->media_type == "document") label = "[Document]";
            else                                   label = "[Media]";
            char buf[48];
            if (it->media_w > 0 && it->media_h > 0) {
                snprintf(buf, sizeof(buf), "%s %dx%d", label, it->media_w, it->media_h);
            } else {
                snprintf(buf, sizeof(buf), "%s", label);
            }
            lv_obj_t *mlbl = lv_label_create(row);
            lv_label_set_text(mlbl, buf);
            lv_obj_set_style_text_color(mlbl, UI_COLOR_MUTED, 0);
            lv_obj_set_style_text_font(mlbl, get_telegram_font(), 0);
        }

        if (!it->text.empty()) {
            lv_obj_t *t = lv_label_create(row);
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
            lv_label_set_text(t, ascii_safe(it->text).c_str());
            lv_obj_set_width(t, lv_pct(100));
            lv_obj_set_style_text_color(t, UI_COLOR_FG, 0);
            lv_obj_set_style_text_font(t, get_telegram_font(), 0);
        }
    }
    lv_obj_scroll_to_y(s_msgs_holder, LV_COORD_MAX, LV_ANIM_OFF);
}

#ifdef ARDUINO
// Tells the bridge the chat has been read up to `up_to_msg_id`. The bridge
// handler requires the up_to field (see tg-bridge chats.go handleMarkRead)
// and proxies to Telegram's messages.readHistory. Fire-and-forget: a
// failure here only means the unread badge lingers on the bridge side, so
// errors are logged via the status pill but don't abort the chat view.
static void mark_chat_read(int up_to_msg_id)
{
    if (s_current_chat_id == 0 || up_to_msg_id <= 0) return;
    if (up_to_msg_id <= s_last_marked_msg_id) return;
    if (s_bg_task != nullptr) return; // skip if bg sync is running

    char path[48];
    snprintf(path, sizeof(path), "/v1/chats/%lld/read", s_current_chat_id);
    char body_buf[48];
    snprintf(body_buf, sizeof(body_buf), "{\"up_to\":%d}", up_to_msg_id);
    std::string resp, err;
    if (tg_post_json(path, std::string(body_buf), resp, &err)) {
        s_last_marked_msg_id = up_to_msg_id;
    }
    // On failure we intentionally leave s_last_marked_msg_id alone so the
    // next successful fetch retries.
}

static bool fetch_messages()
{
    if (s_current_chat_id == 0) return false;
    if (s_bg_task != nullptr) {
        set_status("Syncing in background...", UI_COLOR_MUTED);
        return false;
    }

    std::string body, err;
    char path[80];
    snprintf(path, sizeof(path), "/v1/chats/%lld/messages?limit=%d",
             s_current_chat_id, TG_MSG_LIMIT);
    if (!tg_get(path, body, &err)) {
        set_status(err.c_str(), lv_palette_main(LV_PALETTE_RED));
        return false;
    }
    cJSON *arr = cJSON_Parse(body.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        set_status("Parse error", lv_palette_main(LV_PALETTE_RED));
        if (arr) cJSON_Delete(arr);
        return false;
    }
    s_msgs.clear();
    int n = cJSON_GetArraySize(arr);
    s_msgs.reserve((size_t)n);
    int newest_id = 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        Message m;
        cJSON *jid  = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(it, "text");
        cJSON *jfr  = cJSON_GetObjectItemCaseSensitive(it, "from");
        cJSON *jout = cJSON_GetObjectItemCaseSensitive(it, "out");
        m.id   = (jid  && cJSON_IsNumber(jid))  ? (int)jid->valuedouble : 0;
        m.text = (jtxt && cJSON_IsString(jtxt)) ? jtxt->valuestring : "";
        m.from = (jfr  && cJSON_IsString(jfr))  ? jfr->valuestring : "";
        m.out  = (jout && cJSON_IsBool(jout))   ? cJSON_IsTrue(jout) : false;
        m.media_w = 0;
        m.media_h = 0;
        cJSON *jmd = cJSON_GetObjectItemCaseSensitive(it, "media");
        if (jmd && cJSON_IsObject(jmd)) {
            cJSON *jmt = cJSON_GetObjectItemCaseSensitive(jmd, "type");
            cJSON *jmw = cJSON_GetObjectItemCaseSensitive(jmd, "w");
            cJSON *jmh = cJSON_GetObjectItemCaseSensitive(jmd, "h");
            if (jmt && cJSON_IsString(jmt)) m.media_type = jmt->valuestring;
            if (jmw && cJSON_IsNumber(jmw)) m.media_w = (int)jmw->valuedouble;
            if (jmh && cJSON_IsNumber(jmh)) m.media_h = (int)jmh->valuedouble;
        }
        if (m.id > newest_id) newest_id = m.id;
        s_msgs.push_back(std::move(m));
    }
    cJSON_Delete(arr);
    render_msgs();
    set_status("", UI_COLOR_MUTED);

    // Now that the messages are on screen, tell the bridge the user has
    // seen everything up through the newest id. Skips inside mark_chat_read
    // when nothing has advanced since the last call.
    mark_chat_read(newest_id);
    return true;
}

static bool send_text(const char *text)
{
    if (s_current_chat_id == 0 || !text || !*text) return false;
    if (s_bg_task != nullptr) {
        set_status("Please wait, syncing...", UI_COLOR_ACCENT);
        return false;
    }
    std::string body = json_text_body(text);
    char path[48];
    snprintf(path, sizeof(path), "/v1/chats/%lld/messages", s_current_chat_id);
    std::string resp, err;
    if (!tg_post_json(path, body, resp, &err)) {
        set_status(("send: " + err).c_str(), lv_palette_main(LV_PALETTE_RED));
        return false;
    }
    set_status("Sent", UI_COLOR_MUTED);
    return true;
}
#else
static bool fetch_messages() { return false; }
static bool send_text(const char *) { return false; }
#endif

// --- polling --------------------------------------------------------------

static void poll_tick(lv_timer_t *t)
{
    (void)t;
#ifdef ARDUINO
    if (!configured()) return;
    if (!internet_available()) {
        set_status("No internet", UI_COLOR_MUTED);
        return;
    }
    // Pause fetches while the composer (or any textarea) is focused — HTTPS
    // calls here block the LVGL thread for ~1s and would stall keystrokes.
    if (core::isTextInputFocused()) return;

    if (s_bg_task != nullptr) {
        lv_timer_set_period(t, 1000);
        return;
    }

    switch (s_view) {
        case V_LIST: 
            fetch_chats();
            lv_timer_set_period(t, TG_LIST_POLL_MS);
            break;
        case V_CHAT:
            // Skip while the user is scrolling the history — a refetch
            // re-renders and pins scroll to the bottom, fighting them.
            if (s_msgs_scroll_mode) break;
            fetch_messages();
            lv_timer_set_period(t, TG_CHAT_POLL_MS);
            break;
        default: break;
    }
#endif
}

static void stop_timer()
{
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
}

static void start_timer(uint32_t period_ms)
{
    stop_timer();
    s_timer = lv_timer_create(poll_tick, period_ms, nullptr);
}

// --- view switching -------------------------------------------------------

static void back_to_menu_cb(lv_event_t *) { menu_show(); }
static void back_to_list_cb(lv_event_t *) { show_chat_list(); }

// --- inline composer ------------------------------------------------------

// Collapsed when not focused so the messages area takes the whole view; the
// user rotates onto it to expand and type. Same pattern as ui_tasks.cpp.
static void set_input_collapsed(lv_obj_t *ta, bool collapsed)
{
    if (!ta) return;
    if (collapsed) {
        if (s_input_expanded_h == 0) {
            lv_obj_update_layout(ta);
            s_input_expanded_h = lv_obj_get_height(ta);
        }
        lv_obj_set_height(ta, 0);
        lv_obj_set_style_pad_ver(ta, 0, 0);
        lv_obj_set_style_border_width(ta, 0, 0);
        lv_obj_set_style_opa(ta, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_height(ta,
            s_input_expanded_h > 0 ? s_input_expanded_h : LV_SIZE_CONTENT);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_PAD_TOP, 0);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_PAD_BOTTOM, 0);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_BORDER_WIDTH, 0);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_OPA, 0);
    }
}

static void input_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(ta);

    if (code == LV_EVENT_FOCUSED) {
        set_input_collapsed(ta, false);
        // The message holder shrinks when flex re-lays the column, but its
        // scroll_y stays put, so the newest messages fall off the bottom
        // and look hidden behind the composer. Re-pin to the bottom.
        if (s_msgs_holder) {
            lv_obj_update_layout(s_root);
            lv_obj_scroll_to_y(s_msgs_holder, LV_COORD_MAX, LV_ANIM_OFF);
        }
        // Stay out of visual editing mode — the physical keyboard still
        // delivers characters to the focused textarea, and rotating the
        // encoder should move focus rather than move the caret.
        if (g) lv_group_set_editing(g, false);
    } else if (code == LV_EVENT_DEFOCUSED) {
        set_input_collapsed(ta, true);
        if (s_msgs_holder) {
            lv_obj_update_layout(s_root);
            lv_obj_scroll_to_y(s_msgs_holder, LV_COORD_MAX, LV_ANIM_OFF);
        }
    } else if (code == LV_EVENT_CLICKED) {
        lv_group_focus_obj(ta);
        if (g) lv_group_set_editing(g, false);
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_ENTER) {
            const char *txt = lv_textarea_get_text(ta);
            if (txt && *txt) {
                if (send_text(txt)) {
                    lv_textarea_set_text(ta, "");
                    fetch_messages();
                }
            }
            lv_event_stop_processing(e);
        } else if (key == LV_KEY_ESC) {
            // Route through the shared status-bar back button so teardown
            // matches what the hardware back key does.
            lv_obj_t *bb = core::System::getInstance().getBackButton();
            if (bb) lv_obj_send_event(bb, LV_EVENT_CLICKED, NULL);
            lv_event_stop_processing(e);
        }
    }
}

// --- message-history scroll mode ------------------------------------------

// Clicking the messages container toggles encoder "scroll mode": while
// editing=true the encoder rotation scrolls the list; another click drops
// back to the normal focus chain (back / input / etc.).
static void msgs_holder_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *holder = (lv_obj_t *)lv_event_get_current_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(holder);
    if (!g) return;

    if (code == LV_EVENT_FOCUSED) {
        // Thin accent border while focused so the user can tell the
        // container is selectable; click bumps it to UI_BORDER_W.
        lv_obj_set_style_border_color(holder, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(holder, 1, 0);
    } else if (code == LV_EVENT_CLICKED) {
        bool editing = lv_group_get_editing(g) &&
                       lv_group_get_focused(g) == holder;
        lv_group_set_editing(g, !editing);
        s_msgs_scroll_mode = !editing;
        // Thicker border in scroll mode so the mode change is obvious.
        lv_obj_set_style_border_color(holder, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(holder, !editing ? UI_BORDER_W : 1, 0);
    } else if (code == LV_EVENT_DEFOCUSED) {
        if (lv_group_get_editing(g)) lv_group_set_editing(g, false);
        s_msgs_scroll_mode = false;
        lv_obj_set_style_border_width(holder, 0, 0);
    } else if (code == LV_EVENT_KEY) {
        if (!lv_group_get_editing(g) ||
            lv_group_get_focused(g) != holder) return;
        uint32_t key = lv_event_get_key(e);
        int32_t step = 40;
        if (key == LV_KEY_UP || key == LV_KEY_LEFT) {
            int32_t cur_y = lv_obj_get_scroll_y(holder);
            if (cur_y < step) step = cur_y;
            if (step > 0) lv_obj_scroll_by(holder, 0, step, LV_ANIM_ON);
            lv_event_stop_processing(e);
        } else if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) {
            int32_t bottom = lv_obj_get_scroll_bottom(holder);
            if (bottom < step) step = bottom;
            if (step > 0) lv_obj_scroll_by(holder, 0, -step, LV_ANIM_ON);
            lv_event_stop_processing(e);
        }
    }
}

static void common_page_prep()
{
    if (!s_root) return;
    lv_obj_clean(s_root);
    lv_obj_set_style_bg_color(s_root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 4, 0);
    lv_obj_set_style_pad_row(s_root, 4, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // Drop any stale widget refs from the previous page.
    s_list_holder = nullptr;
    s_msgs_holder = nullptr;
    s_status_label = nullptr;
    s_input_ta = nullptr;
    s_input_expanded_h = 0;
}

static lv_obj_t *make_header(const char *title)
{
    lv_obj_t *hdr = lv_obj_create(s_root);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 2, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(hdr);
    lv_label_set_text(lbl, title);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(lbl, get_telegram_font(), 0);
    lv_obj_set_flex_grow(lbl, 1);
    return hdr;
}

static void show_chat_list()
{
    const bool try_auto_enter = s_auto_enter_single_pending;
    s_auto_enter_single_pending = false;

    s_view = V_LIST;
    s_current_chat_id = 0;
    stop_timer();
    common_page_prep();
    ui_show_back_button(back_to_menu_cb);

    // Zero top padding so the chat list sits flush under the status bar.
    lv_obj_set_style_pad_top(s_root, 0, 0);

    s_list_holder = lv_list_create(s_root);
    lv_obj_set_flex_grow(s_list_holder, 1);
    lv_obj_set_width(s_list_holder, lv_pct(100));
    lv_obj_set_style_bg_opa(s_list_holder, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_holder, 0, 0);
    lv_obj_set_style_pad_top(s_list_holder, 0, 0);

    // Floating status pill at bottom-right, black bg on text only.
    // FLOATING keeps it out of the flex column so it overlays the list.
    s_status_label = lv_label_create(s_root);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_font(s_status_label, get_telegram_font(), 0);
    lv_obj_set_style_text_color(s_status_label, UI_COLOR_FG, 0);
    lv_obj_set_style_bg_color(s_status_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_status_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_status_label, 4, 0);
    lv_obj_set_style_pad_ver(s_status_label, 1, 0);
    lv_obj_set_style_radius(s_status_label, 2, 0);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_RIGHT, -4, -4);

    if (!configured()) {
        set_status("Not configured — Settings → Telegram", UI_COLOR_MUTED);
        return;
    }
#ifdef ARDUINO
    if (!hw_get_wifi_connected()) {
        set_status("WiFi not connected", UI_COLOR_MUTED);
        return;
    }
    if (!internet_available()) {
        set_status("No internet", UI_COLOR_MUTED);
        return;
    }
    bool ok = false;
    if (s_bg_task != nullptr) {
        set_status("Waiting for sync...", UI_COLOR_ACCENT);
        start_timer(1000);
    } else {
        ok = fetch_chats();
        start_timer(TG_LIST_POLL_MS);
    }

    // If the user has pinned exactly one favorite, jump straight into that
    // chat on app entry. Only fires when the fetch returned the chat in the
    // first TG_CHAT_LIMIT results — otherwise we leave the user on the list
    // so they can see the (empty) state rather than a blank chat header.
    if (ok && try_auto_enter && s_favorites.size() == 1) {
        long long fav_id = *s_favorites.begin();
        for (const auto &c : s_chats) {
            if (c.id == fav_id) {
                show_chat(fav_id, c.title.c_str());
                return;
            }
        }
    }
#else
    (void)try_auto_enter;
    set_status("Not supported on emulator.", UI_COLOR_MUTED);
#endif
}

static void show_chat(long long id, const char *title)
{
    s_view = V_CHAT;
    s_current_chat_id = id;
    s_current_chat_title = title ? title : "";
    s_last_marked_msg_id = 0;   // fresh chat → re-arm the mark-read POST
    s_input_expanded_h = 0;
    s_msgs_scroll_mode = false;
    stop_timer();
    common_page_prep();
    // Zero top/row padding so the chat name sits flush under the status bar
    // and the history fills the rest of the screen tightly.
    lv_obj_set_style_pad_top(s_root, 0, 0);
    lv_obj_set_style_pad_row(s_root, 2, 0);
    // Single-favorite flow: the chat list is redundant (only one entry), so
    // back out straight to the home menu. With 0 or >1 favorites the list
    // still makes sense, so go back to it as usual.
    ui_show_back_button(s_favorites.size() == 1 ? back_to_menu_cb
                                                : back_to_list_cb);

    // Non-flex wrapper so the chat-title pill can float on top of the
    // history area instead of taking its own row.
    lv_obj_t *msgs_wrap = lv_obj_create(s_root);
    lv_obj_set_width(msgs_wrap, lv_pct(100));
    lv_obj_set_flex_grow(msgs_wrap, 1);
    lv_obj_set_style_bg_opa(msgs_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(msgs_wrap, 0, 0);
    lv_obj_set_style_pad_all(msgs_wrap, 0, 0);
    lv_obj_clear_flag(msgs_wrap, LV_OBJ_FLAG_SCROLLABLE);

    s_msgs_holder = lv_obj_create(msgs_wrap);
    lv_obj_set_size(s_msgs_holder, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_msgs_holder, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_msgs_holder, 0, 0);
    lv_obj_set_style_border_color(s_msgs_holder, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_msgs_holder, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(s_msgs_holder, 2, 0);
    lv_obj_set_style_pad_row(s_msgs_holder, 3, 0);
    lv_obj_set_flex_flow(s_msgs_holder, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_msgs_holder, LV_DIR_VER);
    lv_obj_add_flag(s_msgs_holder, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(lv_group_get_default(), s_msgs_holder);
    lv_obj_add_event_cb(s_msgs_holder, msgs_holder_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_msgs_holder, msgs_holder_event_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_msgs_holder, msgs_holder_event_cb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(s_msgs_holder, msgs_holder_event_cb, LV_EVENT_DEFOCUSED, nullptr);

    // Floating chat-title pill at the top-right of the history area, with a
    // black background behind the text only.
    lv_obj_t *title_pill = lv_label_create(msgs_wrap);
    lv_label_set_text(title_pill, ascii_safe(s_current_chat_title).c_str());
    lv_obj_set_style_text_color(title_pill, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(title_pill, get_telegram_font(), 0);
    lv_obj_set_style_bg_color(title_pill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(title_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(title_pill, 4, 0);
    lv_obj_set_style_pad_ver(title_pill, 1, 0);
    lv_obj_set_style_radius(title_pill, 2, 0);
    lv_obj_add_flag(title_pill, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(title_pill, LV_ALIGN_TOP_RIGHT, -2, 2);

    // Status label floats at bottom-right of history too, same style, so
    // errors/send feedback don't steal a line of message area.
    s_status_label = lv_label_create(msgs_wrap);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_font(s_status_label, get_telegram_font(), 0);
    lv_obj_set_style_text_color(s_status_label, UI_COLOR_FG, 0);
    lv_obj_set_style_bg_color(s_status_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_status_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_status_label, 4, 0);
    lv_obj_set_style_pad_ver(s_status_label, 1, 0);
    lv_obj_set_style_radius(s_status_label, 2, 0);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_RIGHT, -2, -2);

    s_input_ta = lv_textarea_create(s_root);
    lv_textarea_set_one_line(s_input_ta, true);
    lv_textarea_set_placeholder_text(s_input_ta, "Message...");
    lv_obj_set_width(s_input_ta, lv_pct(100));
    lv_obj_set_style_text_font(s_input_ta, get_telegram_font(), 0);
    lv_obj_set_style_text_color(s_input_ta, UI_COLOR_MUTED, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(s_input_ta, input_event_cb, LV_EVENT_ALL, nullptr);
    lv_group_add_obj(lv_group_get_default(), s_input_ta);
    // Force the expanded height to be measured before collapse so the first
    // focus animates into a real size rather than LV_SIZE_CONTENT.
    lv_obj_update_layout(s_input_ta);
    s_input_expanded_h = lv_obj_get_height(s_input_ta);
    set_input_collapsed(s_input_ta, true);

    s_msgs.clear();
#ifdef ARDUINO
    if (!internet_available()) {
        set_status("No internet", UI_COLOR_MUTED);
        return;
    }
    if (s_bg_task != nullptr) {
        set_status("Waiting for sync...", UI_COLOR_ACCENT);
        start_timer(1000);
    } else {
        fetch_messages();
        start_timer(TG_CHAT_POLL_MS);
    }
#endif
}

// --- not-configured placeholder -------------------------------------------

// When the bridge URL or bearer token are missing the app has nothing to do,
// so instead of the chat list we point the user at the Settings screen
// where the fields now live (Settings → Telegram).
static void show_not_configured()
{
    s_view = V_NOT_CONFIGURED;
    stop_timer();
    common_page_prep();
    ui_show_back_button(back_to_menu_cb);

    make_header("Telegram");

    lv_obj_t *msg = lv_label_create(s_root);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_label_set_text(msg,
        "Not configured.\n\n"
        "Open Settings \xC2\xBB Telegram to set the bridge URL "
        "and bearer token.");
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_color(msg, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(msg, get_telegram_font(), 0);
}

// --- App ------------------------------------------------------------------

static void on_unlocked(bool ok, void *)
{
    if (!ok) {
        // User cancelled or got the passphrase wrong — kick back to menu
        // rather than silently sit on a screen with no token.
        menu_show();
        return;
    }
    reload_config();
    if (configured()) {
        s_auto_enter_single_pending = true;
        show_chat_list();
    } else {
        show_not_configured();
    }
}

class TelegramApp : public core::App {
public:
    TelegramApp() : core::App("Telegram") {}

    void onStart(lv_obj_t *parent) override {
        setRoot(parent);
        s_root = parent;
        reload_config();

#ifdef ARDUINO
        // If the token is at rest encrypted but the session is locked,
        // trigger the notes unlock modal before showing any app content.
        // ui_passphrase_unlock() fires the callback immediately when crypto
        // is disabled or already unlocked, so the common case is free.
        if (s_auth_header.empty() && token_is_encrypted()) {
            common_page_prep();
            s_status_label = lv_label_create(s_root);
            lv_label_set_text(s_status_label, "Unlocking...");
            lv_obj_set_style_text_color(s_status_label, UI_COLOR_MUTED, 0);
            lv_obj_set_style_text_font(s_status_label, get_telegram_font(), 0);
            ui_show_back_button(back_to_menu_cb);
            ui_passphrase_unlock(on_unlocked, nullptr);
            return;
        }
#endif
        if (configured()) {
            s_auto_enter_single_pending = true;
            show_chat_list();
        } else {
            show_not_configured();
        }
    }

    void onStop() override {
        stop_timer();
        ui_hide_back_button();
        s_view = V_NONE;
        s_current_chat_id = 0;
        s_root = nullptr;
        s_status_label = nullptr;
        s_list_holder = nullptr;
        s_msgs_holder = nullptr;
        s_input_ta = nullptr;
        s_chats.clear();
        s_msgs.clear();
        // Wipe the plaintext bearer header; clear() keeps capacity, so we
        // overwrite the buffer before release.
        scrub_string(s_auth_header);
        s_base_url.clear();
        core::App::onStop();
    }
};

// --- background unread-count poll -----------------------------------------

// Fires enabled notifiers for `delta` new unread messages via the shared
// notification bus. The prefs still gate each channel independently — a
// user who wants a toast without a buzz (or vice versa) keeps that control.
// Callable from the bg-poll task; notify::post() is thread-safe.
static void fire_notifications(int delta)
{
    if (delta <= 0) return;
    bool toast = load_bool_pref("notif_toast", true);
    bool vib   = load_bool_pref("notif_vib",   true);

    if (toast) {
        core::notify::Notification n;
        n.icon     = LV_SYMBOL_ENVELOPE;
        if (delta <= 1) {
            n.title = "New Telegram message";
        } else {
            char buf[48];
            snprintf(buf, sizeof(buf), "%d new Telegram messages", delta);
            n.title = buf;
        }
        n.severity = core::notify::Severity::Info;
        n.source   = "telegram";
        n.haptic   = vib;   // bus buzzes once with the banner
        core::notify::post(std::move(n));
    } else if (vib) {
#ifdef ARDUINO
        // Toast disabled but haptic still on — fire directly. hw_feedback
        // no-ops when the global haptic toggle is off.
        hw_feedback();
#endif
    }
}

#ifdef ARDUINO
static void tg_bg_task(void *arg)
{
    std::string url   = load_pref("url");
    std::string token = load_token_plain();
    if (url.empty() || token.empty()) {
        scrub_string(token);
        s_bg_task = nullptr;
        vTaskDelete(NULL);
        return;
    }
    if (!internet_available()) {
        scrub_string(token);
        s_bg_task = nullptr;
        vTaskDelete(NULL);
        return;
    }

    std::string auth = "Bearer " + token;
    scrub_string(token);  // plaintext is now only inside `auth`
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/v1/chats?limit=";
    char n[8]; snprintf(n, sizeof(n), "%d", TG_CHAT_LIMIT); url += n;

    std::string body; int code = 0;
    bool http_ok = hw_http_request(url.c_str(), "GET", nullptr, 0, nullptr,
                                   auth.c_str(), body, &code, nullptr);
    scrub_string(auth);
    if (!http_ok) { 
        s_bg_task = nullptr;
        vTaskDelete(NULL); 
        return; 
    }

    cJSON *arr = cJSON_Parse(body.c_str());
    if (!arr || !cJSON_IsArray(arr)) { 
        if (arr) cJSON_Delete(arr); 
        s_bg_task = nullptr;
        vTaskDelete(NULL); 
        return; 
    }
    int sum = 0;
    int sz  = cJSON_GetArraySize(arr);
    for (int i = 0; i < sz; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *ju = cJSON_GetObjectItemCaseSensitive(it, "unread");
        if (ju && cJSON_IsNumber(ju)) {
            int u = (int)ju->valuedouble;
            if (u > 0) sum += u;
        }
    }
    cJSON_Delete(arr);

    {
        core::ScopedInstanceLock lock;
        s_unread_total = sum;

        // Seed the baseline on the first successful poll so we don't fire on
        // messages that arrived before the device booted. After that, any
        // positive delta triggers the enabled notifiers once.
        if (s_last_notified_unread < 0) {
            s_last_notified_unread = sum;
        } else if (sum > s_last_notified_unread) {
            fire_notifications(sum - s_last_notified_unread);
            s_last_notified_unread = sum;
        } else if (sum < s_last_notified_unread) {
            // Chats read elsewhere — follow the count down so we re-arm cleanly.
            s_last_notified_unread = sum;
        }
    }

    s_bg_task = nullptr;
    vTaskDelete(NULL);
}

static void tg_bg_tick(lv_timer_t *t)
{
    (void)t;
    if (s_view != V_NONE) return;               // app is open → its timer drives updates
    if (!internet_available()) return;
    // Skip while typing elsewhere: this tick runs an HTTPS fetch (~1s).
    if (core::isTextInputFocused()) return;

    if (s_bg_task != nullptr) return; // Previous task still running

    // Launch background task to avoid blocking the LVGL timer thread
    xTaskCreate(tg_bg_task, "tg_bg", 6144, nullptr, 2, &s_bg_task);
}
#endif

} // namespace

namespace apps {
APP_FACTORY(make_telegram_app, TelegramApp)

int tg_get_unread_count() { return s_unread_total; }

// --- config helpers called from ui_settings.cpp ---------------------------

std::string tg_cfg_get_url()
{
    return load_pref("url");
}

void tg_cfg_set_url(const char *url)
{
    save_pref("url", url ? url : "");
    if (s_view != V_NONE) reload_config();
}

// Returns a masked representation suitable for display: "(not set)",
// "(locked)" when the token is encrypted but the notes session isn't
// unlocked, or "********abcd" showing the last four characters.
std::string tg_cfg_get_token_display()
{
    std::string tok = load_token_plain();
    if (tok.empty()) {
        return token_is_encrypted() ? "(locked)" : "(not set)";
    }
    if (tok.size() <= 8) return "********";
    return std::string("********") + tok.substr(tok.size() - 4);
}

bool tg_cfg_set_token(const char *tok, std::string *err)
{
    bool ok = save_token(tok ? tok : "", err);
    if (ok && s_view != V_NONE) reload_config();
    return ok;
}

bool tg_cfg_token_is_encrypted()
{
    return token_is_encrypted();
}

// Per-channel notification toggles. Stored in the same tgbridge NVS namespace
// so they travel with the rest of the Telegram config. Defaults are ON so
// the user is notified out of the box — the Settings subpage can turn them
// off individually.
bool tg_cfg_get_notif_vibrate() { return load_bool_pref("notif_vib",   true); }
void tg_cfg_set_notif_vibrate(bool on) { save_bool_pref("notif_vib",   on); }
bool tg_cfg_get_notif_banner()  { return load_bool_pref("notif_toast", true); }
void tg_cfg_set_notif_banner(bool on)  { save_bool_pref("notif_toast", on); }

bool tg_cfg_is_favorite(long long id)
{
    if (!s_favorites_loaded) load_favorites();
    return s_favorites.find(id) != s_favorites.end();
}

void tg_cfg_set_favorite(long long id, bool on)
{
    if (!s_favorites_loaded) load_favorites();
    bool changed = false;
    if (on) {
        changed = s_favorites.insert(id).second;
    } else {
        changed = s_favorites.erase(id) > 0;
    }
    if (changed) save_favorites();
    // If the chat list view is on screen, re-render so the filter is
    // immediately reflected when the user comes back from settings.
    if (s_view == V_LIST) render_chats();
}

bool tg_cfg_fetch_all_chats(std::vector<std::pair<long long, std::string>> &out,
                            std::string *err)
{
    out.clear();
#ifdef ARDUINO
    std::string url = load_pref("url");
    std::string tok = load_token_plain();
    if (url.empty()) { scrub_string(tok); if (err) *err = "Bridge URL not set."; return false; }
    if (tok.empty()) {
        if (err) *err = token_is_encrypted()
                        ? "Token locked — open Notes to unlock."
                        : "Bearer token not set.";
        return false;
    }
    if (!internet_available()) {
        scrub_string(tok);
        if (err) *err = hw_get_wifi_connected() ? "No internet" : "WiFi not connected";
        return false;
    }
    std::string auth = "Bearer " + tok;
    scrub_string(tok);  // plaintext is now only inside `auth`
    if (!url.empty() && url.back() == '/') url.pop_back();
    char limitpart[32];
    snprintf(limitpart, sizeof(limitpart), "/v1/chats?limit=%d", TG_CHAT_LIMIT);
    std::string full = url + limitpart;
    std::string body;
    int code = 0;
    bool http_ok = hw_http_request(full.c_str(), "GET", nullptr, 0, nullptr,
                                   auth.c_str(), body, &code, err);
    scrub_string(auth);
    if (!http_ok) return false;
    cJSON *arr = cJSON_Parse(body.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        if (err) *err = "Parse error";
        return false;
    }
    int n = cJSON_GetArraySize(arr);
    out.reserve((size_t)n);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jti = cJSON_GetObjectItemCaseSensitive(it, "title");
        long long id = (jid && cJSON_IsNumber(jid)) ? (long long)jid->valuedouble : 0;
        std::string title = (jti && cJSON_IsString(jti)) ? jti->valuestring : "(no title)";
        if (id != 0) out.emplace_back(id, std::move(title));
    }
    cJSON_Delete(arr);
    return true;
#else
    (void)out;
    if (err) *err = "Not supported on emulator.";
    return false;
#endif
}

void tg_begin_background_poll() {
#ifdef ARDUINO
    // First call at boot: drop any plaintext bearer a pre-hardening build
    // may have left. Cheap no-op afterwards so the idempotent double-call
    // pattern this function already tolerates still holds.
    purge_legacy_plaintext_token();
    if (s_bg_timer) return;
    // 60s cadence — the HTTP call blocks the LVGL thread for ~1s on a good
    // connection, so we keep the frequency low to stay invisible to the
    // rest of the UI.
    s_bg_timer = lv_timer_create(tg_bg_tick, 60000, nullptr);
#endif
}
} // namespace apps
