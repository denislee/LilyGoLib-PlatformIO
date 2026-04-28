/**
 * @file      ui_chat.cpp
 * @brief     LLM chat client backed by lilyhub /api/chat (Groq STT + LLM).
 *
 * The app gives the user two ways to ask a question — type it, or hold
 * the mic — and renders the model's reply as a wrapped text line in a
 * scrolling log. The on-device firmware does not talk to Groq directly:
 * everything goes through the LAN hub, which holds the API key and runs
 * Whisper + chat completion on a real CPU.
 *
 * No fallback path: unlike notes-sync, there is no useful direct route
 * (we'd have to embed a Groq key on the device, defeating the point of
 * the hub). When the hub is disabled or unreachable the app surfaces a
 * clean error and does nothing else.
 */
#include "../ui_define.h"
#include "../hal/hub.h"
#include "../hal/wireless.h"
#include "../hal/storage.h"
#include "../hal/audio.h"
#include "../hal/system.h"
#include "../core/app.h"
#include "../core/app_manager.h"
#include "../core/scoped_lock.h"
#include "app_registry.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <LilyGoLib.h>
#include <mbedtls/base64.h>
extern "C" {
#include "cJSON.h"
}
#endif

namespace {

static const char *CHAT_DIR        = "/chat";
static const char *DEFAULT_DEVICE  = "pager-01";

// LVGL widgets — single-instance, lifetime tied to enter()/exit_cb().
static lv_obj_t *s_root        = nullptr;
static lv_obj_t *s_log_scroll  = nullptr;
static lv_obj_t *s_log_label   = nullptr;
static lv_obj_t *s_input_ta    = nullptr;
static lv_obj_t *s_mic_btn     = nullptr;
static lv_obj_t *s_status_lbl  = nullptr;
static lv_timer_t *s_poll_timer = nullptr;

static std::string s_log_text;
static std::string s_pending_audio_path;
static bool        s_recording = false;
static bool        s_busy      = false;

// --- async send context ---------------------------------------------------
//
// The send task runs off the LVGL thread because hw_http_request is
// blocking. The UI hands ownership of the context to the task; the task
// fills in the result and flips `done` under the instance lock; the LVGL
// poll timer picks it up next tick.
//
// `abandoned` solves the exit race: if the user backs out while a request
// is in flight, the UI marks the context abandoned (still under the lock)
// and the task deletes the context itself when done. Conversely, if the
// task has already finished before exit, the UI sees `done==true` and
// deletes it. The lock disambiguates the two paths.
struct SendCtx {
    std::string url;
    std::string body;
    std::string transcript;
    std::string reply;
    std::string error;
    uint32_t    start_ms = 0;
    bool ok        = false;
    bool done      = false;
    bool abandoned = false;
};
static SendCtx *s_ctx = nullptr;

// Watchdog: if the worker task hasn't flipped done within this window, the
// HTTP call is almost certainly stuck (Arduino HTTPClient.getString() has
// been seen hanging on certain server response shapes). We surface it as
// a visible error and abandon the task so the UI doesn't sit on
// "thinking..." forever.
static const uint32_t SEND_WATCHDOG_MS = 20000;

#ifdef ARDUINO
static TaskHandle_t s_send_task = nullptr;

static bool b64_encode(const uint8_t *data, size_t len, std::string &out)
{
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    out.resize(olen);
    size_t written = 0;
    int rc = mbedtls_base64_encode((unsigned char *)&out[0], out.size(),
                                   &written, data, len);
    if (rc != 0) { out.clear(); return false; }
    out.resize(written);
    return true;
}
#endif

static std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

static void log_append(const char *prefix, const std::string &text)
{
    if (!s_log_label) return;
    if (!s_log_text.empty()) s_log_text.push_back('\n');
    if (prefix) s_log_text.append(prefix);
    s_log_text.append(text);
    lv_label_set_text(s_log_label, s_log_text.c_str());
    if (s_log_scroll) {
        lv_obj_update_layout(s_log_scroll);
        lv_obj_scroll_to_y(s_log_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void set_status(const char *txt)
{
    if (!s_status_lbl) return;
    if (txt && *txt) {
        lv_label_set_text(s_status_lbl, txt);
        lv_obj_remove_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_status_lbl, "");
        lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_busy(bool busy)
{
    s_busy = busy;

    // Crucial ordering: we must un-hide the status label BEFORE asking
    // the group to focus it. lv_group_focus_obj() is a no-op on hidden
    // objects, so if we left the label hidden the focus would stay on
    // the textarea — which we're about to disable — and both keyboard
    // and encoder would have no live target to deliver events to.
    if (busy && s_status_lbl) {
        lv_label_set_text(s_status_lbl, "thinking... (tap to cancel)");
        lv_obj_remove_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    auto apply = [&](lv_obj_t *o) {
        if (!o) return;
        if (busy) lv_obj_add_state(o, LV_STATE_DISABLED);
        else      lv_obj_remove_state(o, LV_STATE_DISABLED);
    };
    apply(s_mic_btn);
    apply(s_input_ta);

    if (s_status_lbl && s_input_ta) {
        if (busy) {
            lv_group_focus_obj(s_status_lbl);
        } else {
            lv_group_focus_obj(s_input_ta);
        }
    }
}

// Detach an in-flight send. The HTTP call itself is blocking and lives in
// the worker task — we can't truly abort it, but we can stop caring about
// the result: clear our pointer, mark the ctx abandoned, and let the task
// free itself when it eventually returns.
static void cancel_inflight()
{
    if (!s_ctx) return;
    bool delete_now;
    {
        core::ScopedInstanceLock lock;
        if (s_ctx->done) {
            delete_now = true;
        } else {
            s_ctx->abandoned = true;
            delete_now = false;
        }
    }
    if (delete_now) delete s_ctx;
    s_ctx = nullptr;
}

static void status_click_cb(lv_event_t *)
{
    if (!s_busy) return;
    cancel_inflight();
    set_busy(false);
    log_append("info: ", "cancelled");
    set_status("");
}

static std::string current_device_id()
{
    // A pager is single-user; one fixed ID is sufficient for the hub to
    // key history. If multiple pagers ever share one hub we'll derive
    // this from the WiFi MAC.
    return DEFAULT_DEVICE;
}

#ifdef ARDUINO
static void send_task(void *arg)
{
    SendCtx *ctx = (SendCtx *)arg;

    Serial.printf("[chat] POST %s body=%u bytes\n",
                  ctx->url.c_str(), (unsigned)ctx->body.size());

    std::string resp;
    int code = 0;
    std::string terr;
    bool http_ok = hw_http_request(ctx->url.c_str(), "POST",
                                   ctx->body.c_str(), ctx->body.size(),
                                   "application/json",
                                   nullptr, resp, &code, &terr);
    // Drop the request body promptly — base64 of a voice memo can be
    // hundreds of KB and we no longer need it.
    ctx->body.clear();
    ctx->body.shrink_to_fit();

    Serial.printf("[chat] http_ok=%d code=%d resp=%u bytes terr='%s'\n",
                  (int)http_ok, code, (unsigned)resp.size(), terr.c_str());
    if (!resp.empty()) {
        Serial.printf("[chat] resp[0..160]: %s\n",
                      resp.substr(0, 160).c_str());
    }

    if (!http_ok || code / 100 != 2) {
        if (!terr.empty()) {
            ctx->error = terr;
        } else {
            char buf[24];
            snprintf(buf, sizeof(buf), "HTTP %d", code);
            ctx->error = buf;
        }
        if (!resp.empty()) ctx->error += ": " + resp.substr(0, 160);
    } else {
        cJSON *j = cJSON_Parse(resp.c_str());
        if (!j) {
            ctx->error = "bad json from hub: ";
            ctx->error += resp.substr(0, 120);
        } else {
            cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "transcript");
            cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "reply");
            if (t && cJSON_IsString(t) && t->valuestring) ctx->transcript = t->valuestring;
            if (r && cJSON_IsString(r) && r->valuestring) ctx->reply      = r->valuestring;
            cJSON_Delete(j);
            // Surface the silent-success case: 2xx + parseable JSON but
            // neither field populated. Without this, the UI just sits on
            // "thinking..." and never logs anything.
            if (ctx->reply.empty() && ctx->transcript.empty()) {
                ctx->error = "hub returned no reply: ";
                ctx->error += resp.substr(0, 120);
            } else {
                ctx->ok = true;
            }
        }
    }

    bool free_self;
    {
        core::ScopedInstanceLock lock;
        ctx->done = true;
        free_self = ctx->abandoned;
    }
    if (free_self) delete ctx;
    s_send_task = nullptr;
    vTaskDelete(NULL);
}
#endif

static void poll_timer_cb(lv_timer_t *)
{
    if (!s_ctx) return;
    bool done;
    {
        core::ScopedInstanceLock lock;
        done = s_ctx->done;
    }
#ifdef ARDUINO
    if (!done) {
        if (s_ctx->start_ms != 0) {
            uint32_t elapsed = millis() - s_ctx->start_ms;
            // Watchdog — surface stuck HTTP calls instead of leaving the
            // UI on "thinking..." indefinitely. The worker task keeps
            // running but its result will be discarded.
            if (elapsed > SEND_WATCHDOG_MS) {
                Serial.println("[chat] watchdog: send_task stuck >20s, abandoning");
                cancel_inflight();
                set_busy(false);
                log_append("err: ", "request timeout (no response in 20s)");
                set_status("");
                return;
            }
            // Live progress — gives the user feedback that the UI is
            // alive and counts up to the watchdog. Updated every poll
            // tick (~200ms) but the seconds value only changes once a
            // second so it's not noisy.
            char buf[48];
            snprintf(buf, sizeof(buf),
                     "thinking %us... (tap to cancel)",
                     (unsigned)(elapsed / 1000));
            set_status(buf);
        }
        return;
    }
#else
    if (!done) return;
#endif

    SendCtx *ctx = s_ctx;
    s_ctx = nullptr;

#ifdef ARDUINO
    Serial.printf("[chat] poll: ok=%d reply=%u transcript=%u err='%s'\n",
                  (int)ctx->ok, (unsigned)ctx->reply.size(),
                  (unsigned)ctx->transcript.size(), ctx->error.c_str());
#endif

    if (ctx->ok) {
        if (!ctx->transcript.empty()) {
            // Voice path — the user line was deferred until we knew what
            // we heard, so render it now ahead of the reply.
            log_append("you: ", ctx->transcript);
        }
        if (!ctx->reply.empty()) log_append("ai:  ", ctx->reply);
        set_status("");
    } else {
        log_append("err: ", ctx->error);
        set_status("");
    }
    set_busy(false);
    delete ctx;
}

static void kick_off_send(const std::string &user_text,
                          const std::string &audio_b64)
{
#ifdef ARDUINO
    std::string hub = hal::hub_get_url();
    if (hub.empty()) {
        log_append("err: ", "hub not configured (Settings " "\xC2\xBB" " Local Hub)");
        set_busy(false);
        return;
    }
    if (!hw_get_wifi_connected()) {
        log_append("err: ", "WiFi not connected");
        set_busy(false);
        return;
    }

    SendCtx *ctx = new SendCtx();
    ctx->url = hub + "/api/chat";
    ctx->body.reserve(256 + audio_b64.size() + user_text.size() * 2);
    ctx->body += "{\"device_id\":\"";
    ctx->body += json_escape(current_device_id());
    ctx->body += "\"";
    if (!user_text.empty()) {
        ctx->body += ",\"text\":\"";
        ctx->body += json_escape(user_text);
        ctx->body += "\"";
    }
    if (!audio_b64.empty()) {
        // base64 alphabet doesn't need JSON-escaping.
        ctx->body += ",\"audio_b64\":\"";
        ctx->body += audio_b64;
        ctx->body += "\"";
    }
    ctx->body += "}";

    s_ctx = ctx;
#ifdef ARDUINO
    ctx->start_ms = millis();
#endif
    set_status("thinking... (tap to cancel)");
    if (xTaskCreate(send_task, "chat_send", 8192, ctx, 4, &s_send_task) != pdPASS) {
        s_ctx = nullptr;
        delete ctx;
        log_append("err: ", "task spawn failed");
        set_busy(false);
    }
#else
    (void)user_text; (void)audio_b64;
    log_append("err: ", "chat requires hardware build");
    set_busy(false);
#endif
}

static void send_btn_cb(lv_event_t *)
{
    if (s_busy || !s_input_ta) return;
    const char *raw = lv_textarea_get_text(s_input_ta);
    std::string txt = raw ? raw : "";
    size_t a = txt.find_first_not_of(" \t\r\n");
    size_t b = txt.find_last_not_of(" \t\r\n");
    txt = (a == std::string::npos) ? std::string()
                                   : txt.substr(a, b - a + 1);
    if (txt.empty()) return;
    set_busy(true);
    log_append("you: ", txt);
    lv_textarea_set_text(s_input_ta, "");
    kick_off_send(txt, std::string());
}

#ifdef ARDUINO
static std::string build_chat_audio_path()
{
    struct tm info = {};
    hw_get_date_time(info);
    char buf[64];
    snprintf(buf, sizeof(buf),
             "%s/utt_%04d%02d%02d_%02d%02d%02d.wav",
             CHAT_DIR,
             info.tm_year + 1900, info.tm_mon + 1, info.tm_mday,
             info.tm_hour, info.tm_min, info.tm_sec);
    return buf;
}

static void ensure_chat_dir()
{
    if (!(HW_SD_ONLINE & hw_get_device_online())) return;
    if (!SD.exists(CHAT_DIR)) SD.mkdir(CHAT_DIR);
}
#endif

static void mic_btn_cb(lv_event_t *)
{
    if (s_busy) return;
#ifdef ARDUINO
    if (!s_recording) {
        if (!hw_mic_available()) {
            log_append("err: ", "no microphone");
            return;
        }
        if (!(HW_SD_ONLINE & hw_get_device_online())) {
            log_append("err: ", "SD card not mounted");
            return;
        }
        ensure_chat_dir();
        s_pending_audio_path = build_chat_audio_path();
        if (!hw_rec_start(s_pending_audio_path.c_str())) {
            log_append("err: ", "recording start failed");
            return;
        }
        s_recording = true;
        set_status("recording... tap mic to stop");
        if (s_mic_btn) {
            lv_obj_set_style_bg_color(s_mic_btn,
                lv_palette_main(LV_PALETTE_RED), 0);
        }
        return;
    }

    // Stop, slurp the WAV off the SD card, base64 it, hand to the task.
    if (hw_rec_running()) hw_rec_stop();
    s_recording = false;
    if (s_mic_btn) {
        lv_obj_set_style_bg_color(s_mic_btn, UI_COLOR_ACCENT, 0);
    }
    set_status("transcribing...");
    set_busy(true);

    std::vector<uint8_t> bytes;
    if (!hw_read_sd_bytes_raw(s_pending_audio_path.c_str(), bytes) ||
        bytes.size() <= 44) {
        log_append("err: ", "no audio captured");
        set_busy(false);
        return;
    }
    std::string b64;
    if (!b64_encode(bytes.data(), bytes.size(), b64)) {
        log_append("err: ", "base64 failed");
        set_busy(false);
        return;
    }
    kick_off_send(std::string(), b64);
#else
    log_append("err: ", "mic only on hardware");
#endif
}

static void back_btn_cb(lv_event_t *) { menu_show(); }

static void enter(lv_obj_t *parent)
{
    s_root = parent;
    s_log_text.clear();
    s_busy = false;
    s_recording = false;
    enable_keyboard();
    ui_show_back_button(back_btn_cb);

    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 4, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Conversation log — same look as notes-sync's scrolling log.
    s_log_scroll = lv_obj_create(parent);
    lv_obj_set_width(s_log_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_log_scroll, 1);
    lv_obj_set_style_bg_color(s_log_scroll, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(s_log_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_log_scroll, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_log_scroll, 1, 0);
    lv_obj_set_style_radius(s_log_scroll, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(s_log_scroll, 6, 0);
    lv_obj_set_scroll_dir(s_log_scroll, LV_DIR_VER);

    s_log_label = lv_label_create(s_log_scroll);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_log_label, lv_pct(100));
    lv_obj_set_style_text_color(s_log_label, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_log_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_log_label, "");

    // Status line — "thinking...", "recording...", error hints.
    s_status_lbl = lv_label_create(parent);
    lv_obj_set_width(s_status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_status_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_status_lbl, get_small_font(), 0);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_status_lbl, status_click_cb, LV_EVENT_CLICKED, nullptr);
    // Add to the input group so the encoder/scrollwheel can focus and
    // press it — set_busy() shifts focus here while a request is in
    // flight so the user can cancel without a touchscreen.
    lv_group_add_obj(lv_group_get_default(), s_status_lbl);

    // Input row.
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_input_ta = lv_textarea_create(row);
    lv_textarea_set_placeholder_text(s_input_ta, "Ask anything...");
    lv_textarea_set_one_line(s_input_ta, true);
    lv_textarea_set_max_length(s_input_ta, 500);
    lv_obj_set_flex_grow(s_input_ta, 1);
    lv_obj_set_style_radius(s_input_ta, UI_RADIUS, 0);
    lv_group_add_obj(lv_group_get_default(), s_input_ta);

    auto make_btn = [&](const char *icon, lv_event_cb_t cb) -> lv_obj_t * {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_height(b, 40);
        lv_obj_set_style_bg_color(b, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_radius(b, UI_RADIUS, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, icon);
        lv_obj_set_style_text_color(l, UI_COLOR_FG, 0);
        lv_obj_center(l);
        lv_group_add_obj(lv_group_get_default(), b);
        return b;
    };
    s_mic_btn  = make_btn(LV_SYMBOL_AUDIO, mic_btn_cb);

    // One-line textarea fires LV_EVENT_READY when Enter is pressed (or
    // when the on-screen keyboard's OK key is tapped). Treat that as send.
    lv_obj_add_event_cb(s_input_ta, send_btn_cb, LV_EVENT_READY, nullptr);

    if (hal::hub_get_url().empty()) {
        log_append("info: ",
                   "hub disabled. Settings " "\xC2\xBB" " Local Hub.");
    }

    if (!s_poll_timer) {
        s_poll_timer = lv_timer_create(poll_timer_cb, 200, nullptr);
    }
}

static void exit_cb()
{
    ui_hide_back_button();
    disable_keyboard();
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = nullptr;
    }
#ifdef ARDUINO
    if (s_recording && hw_rec_running()) hw_rec_stop();
#endif
    s_recording = false;

    // Hand off any in-flight context to the task. If the task already
    // finished (done=true) we own the deletion; otherwise the task will
    // see abandoned=true under the same lock and free the buffer itself.
    if (s_ctx) {
        bool delete_now;
        {
            core::ScopedInstanceLock lock;
            if (s_ctx->done) {
                delete_now = true;
            } else {
                s_ctx->abandoned = true;
                delete_now = false;
            }
        }
        if (delete_now) delete s_ctx;
        s_ctx = nullptr;
    }

    s_root = s_log_scroll = s_log_label = nullptr;
    s_input_ta = s_mic_btn = s_status_lbl = nullptr;
    s_log_text.clear();
    s_pending_audio_path.clear();
    s_busy = false;
}

class ChatApp : public core::App {
public:
    ChatApp() : core::App("Chat") {}
    void onStart(lv_obj_t *parent) override {
        setRoot(parent);
        enter(parent);
    }
    void onStop() override {
        exit_cb();
        core::App::onStop();
    }
};

} // namespace

namespace apps {
APP_FACTORY(make_chat_app, ChatApp)
}
