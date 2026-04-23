/**
 * @file      ui_nfc_test.cpp
 * @brief     NFC discovery self-test overlay.
 *
 * Pops up from Settings » Connectivity » NFC Test. Forces the NFC chip on,
 * hooks the low-level detection callback, and reports live counters + the
 * last detected NDEF record type so the user can tell whether:
 *
 *   - the chip is powered and discovery is running
 *   - tags are being detected at all
 *   - records are being parsed by the library
 *   - the provisioning handler is seeing the payload
 *
 * Restores the prior NFC enable state on close.
 */
#include "ui_define.h"
#include "hal/peripherals.h"
#include "core/input_focus.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct NfcTestCtx {
    lv_obj_t *overlay      = nullptr;
    lv_obj_t *state_label  = nullptr;
    lv_obj_t *count_label  = nullptr;
    lv_obj_t *last_label   = nullptr;
    lv_obj_t *hint_label   = nullptr;
    lv_obj_t *reset_btn    = nullptr;
    lv_group_t *group      = nullptr;
    lv_group_t *prev_group = nullptr;
    lv_event_cb_t prev_back_cb = nullptr;
    bool prev_nfc_enabled  = false;

    /* Counters (updated inside diag_hook; read by poll_cb). int is atomic on
     * ESP32-S3 reads/writes so we don't need a lock for these scalars. */
    volatile uint32_t raw_detects = 0;
    volatile uint32_t parsed_text = 0;
    volatile uint32_t parsed_uri  = 0;
    volatile uint32_t parsed_wifi = 0;
    volatile uint32_t parsed_other = 0;

    /* Last event description, written by diag_hook and rendered by poll_cb
     * to keep LVGL calls off the callback (which can fire under the NFC
     * state machine). Bounded char[] rather than std::string so we don't
     * allocate inside the hook. */
    volatile uint32_t last_seq = 0;    /* bumped on every hook invocation */
    uint32_t rendered_seq = 0;
    int      last_kind = -1;
    char     last_preview[48] = {0};

    lv_timer_t *poll_timer = nullptr;
    uint32_t    opened_tick_ms = 0;
    uint32_t    heartbeat = 0;
};

static NfcTestCtx *g_ctx = nullptr;

static const char *kind_name(int kind)
{
    switch (kind) {
        case 0: return "raw";
        case 1: return "Text";
        case 2: return "URI";
        case 3: return "WiFi";
        case 4: return "Other";
        default: return "?";
    }
}

/* Redact credential payloads so a Settings walker doesn't get a free look
 * at the next bearer someone taps in. Text records starting with our
 * provisioning prefix are masked entirely; everything else gets a short
 * preview clipped to the buffer. */
static void fill_preview(char *out, size_t out_sz, int kind,
                         const char *text, unsigned len)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!text || len == 0) return;

    static const char kPrefix[] = "lilygo+";
    const size_t prefix_len = sizeof(kPrefix) - 1;
    if (kind == 1 && len >= prefix_len &&
        memcmp(text, kPrefix, prefix_len) == 0) {
        snprintf(out, out_sz, "<provisioning payload redacted>");
        return;
    }

    size_t copy = len < out_sz - 1 ? len : out_sz - 1;
    /* Replace non-printables with '.' so a binary tag doesn't corrupt the
     * label. */
    for (size_t i = 0; i < copy; i++) {
        unsigned char c = (unsigned char)text[i];
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[copy] = '\0';
}

static void diag_hook(int kind, const char *text, unsigned len)
{
    NfcTestCtx *ctx = g_ctx;
    if (!ctx) return;
    switch (kind) {
        case 0: ctx->raw_detects++;  break;
        case 1: ctx->parsed_text++;  break;
        case 2: ctx->parsed_uri++;   break;
        case 3: ctx->parsed_wifi++;  break;
        case 4: ctx->parsed_other++; break;
    }
    ctx->last_kind = kind;
    fill_preview(ctx->last_preview, sizeof(ctx->last_preview), kind, text, len);
    ctx->last_seq++;
}

static void render(NfcTestCtx *ctx)
{
    if (!ctx) return;

    if (ctx->state_label) {
        /* Three independent signals:
         *  - enable flag   = user setting + power gate
         *  - discovery     = did beginNFC() actually start RFAL polling
         *  - heartbeat     = this overlay is live (rotates every render) */
        static const char spin[] = {'|', '/', '-', '\\'};
        char buf[128];
        uint32_t secs = (lv_tick_get() - ctx->opened_tick_ms) / 1000;
        snprintf(buf, sizeof(buf),
                 "Chip: %s  |  Discovery: %s  %c\n"
                 "Uptime: %u:%02u",
                 hw_get_nfc_enable() ? "ON" : "OFF",
                 hw_nfc_discovery_active() ? "ACTIVE" : "not init",
                 spin[ctx->heartbeat++ & 3],
                 (unsigned)(secs / 60), (unsigned)(secs % 60));
        lv_label_set_text(ctx->state_label, buf);
    }

    if (ctx->count_label) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "Raw: %u  Text: %u  URI: %u\n"
                 "WiFi: %u  Other: %u",
                 (unsigned)ctx->raw_detects,
                 (unsigned)ctx->parsed_text,
                 (unsigned)ctx->parsed_uri,
                 (unsigned)ctx->parsed_wifi,
                 (unsigned)ctx->parsed_other);
        lv_label_set_text(ctx->count_label, buf);
    }

    if (ctx->last_label) {
        if (ctx->last_kind < 0) {
            lv_label_set_text(ctx->last_label, "Last: (none)");
        } else {
            char buf[96];
            if (ctx->last_preview[0]) {
                snprintf(buf, sizeof(buf), "Last: %s — %s",
                         kind_name(ctx->last_kind), ctx->last_preview);
            } else {
                snprintf(buf, sizeof(buf), "Last: %s",
                         kind_name(ctx->last_kind));
            }
            lv_label_set_text(ctx->last_label, buf);
        }
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    NfcTestCtx *ctx = g_ctx;
    if (!ctx) return;
    /* Redraw unconditionally so the heartbeat/uptime visibly updates even
     * when no tag has been detected. Cheap — labels and a timer tick only. */
    ctx->rendered_seq = ctx->last_seq;
    render(ctx);
}

static void tear_down()
{
    if (!g_ctx) return;
    NfcTestCtx *ctx = g_ctx;
    g_ctx = nullptr;

    hw_set_nfc_diag_hook(nullptr);

    /* Restore the NFC enable state we captured at open. If it was off when
     * the user came in, flip it back off so we don't drain battery. */
    if (!ctx->prev_nfc_enabled && hw_get_nfc_enable()) {
        hw_set_nfc_enable(false);
    }

    if (ctx->poll_timer) lv_timer_del(ctx->poll_timer);
    if (ctx->overlay)    lv_obj_del(ctx->overlay);
    if (ctx->group)      lv_group_del(ctx->group);
    if (ctx->prev_group) set_default_group(ctx->prev_group);
    if (ctx->prev_back_cb) ui_show_back_button(ctx->prev_back_cb);
    delete ctx;
}

static void close_event_cb(lv_event_t *e) { (void)e; tear_down(); }

static void reset_event_cb(lv_event_t *e)
{
    (void)e;
    NfcTestCtx *ctx = g_ctx;
    if (!ctx) return;
    ctx->raw_detects = 0;
    ctx->parsed_text = 0;
    ctx->parsed_uri  = 0;
    ctx->parsed_wifi = 0;
    ctx->parsed_other = 0;
    ctx->last_kind = -1;
    ctx->last_preview[0] = '\0';
    ctx->last_seq++;
    hw_feedback();
}

static lv_obj_t *build_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_style_radius(btn, UI_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x151515), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a1a00), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, UI_COLOR_FG, 0);
    lv_obj_center(lbl);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

} /* namespace */

void ui_nfc_test_open()
{
    if (g_ctx) return;

#if !defined(USING_ST25R3916)
    ui_msg_pop_up("NFC Test", "This build has no NFC hardware.");
    return;
#else
    NfcTestCtx *ctx = new NfcTestCtx();
    g_ctx = ctx;

    ctx->prev_group = lv_group_get_default();
    ctx->prev_back_cb = ui_get_back_button_cb();
    ctx->prev_nfc_enabled = hw_get_nfc_enable();
    ctx->group = lv_group_create();
    lv_group_set_wrap(ctx->group, false);
    set_default_group(ctx->group);
    ui_show_back_button(close_event_cb);
    enable_keyboard();

    /* Force discovery on regardless of the user's prior setting. tear_down
     * restores the original state. */
    if (!ctx->prev_nfc_enabled) hw_set_nfc_enable(true);

    const lv_font_t *header_font = get_header_font();
    int32_t bar_h = lv_font_get_line_height(header_font) + 8;
    if (bar_h < 30) bar_h = 30;
    int32_t v_res = lv_display_get_vertical_resolution(NULL);
    if (v_res <= 0) v_res = 222;

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    ctx->overlay = overlay;
    lv_obj_set_size(overlay, lv_pct(100), v_res - bar_h);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 8, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(overlay, 6, 0);

    lv_obj_t *title = lv_label_create(overlay);
    lv_label_set_text(title, "NFC Self-Test");
    lv_obj_set_style_text_color(title, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(title), 0);

    ctx->state_label = lv_label_create(overlay);
    lv_label_set_text(ctx->state_label, "");
    lv_obj_set_style_text_color(ctx->state_label, UI_COLOR_FG, 0);
    lv_label_set_long_mode(ctx->state_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->state_label, lv_pct(100));

    ctx->count_label = lv_label_create(overlay);
    lv_label_set_text(ctx->count_label, "");
    lv_obj_set_style_text_color(ctx->count_label, UI_COLOR_FG, 0);
    lv_label_set_long_mode(ctx->count_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->count_label, lv_pct(100));

    ctx->last_label = lv_label_create(overlay);
    lv_label_set_text(ctx->last_label, "Last: (none)");
    lv_obj_set_style_text_color(ctx->last_label, UI_COLOR_FG, 0);
    lv_label_set_long_mode(ctx->last_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->last_label, lv_pct(100));

    ctx->hint_label = lv_label_create(overlay);
    lv_label_set_text(ctx->hint_label,
        "Tap a tag to the back of the device.\n"
        "\"Raw\" should bump on any tag; \"Text\"\n"
        "shows a lilygo+ tag was recognised.");
    lv_obj_set_style_text_color(ctx->hint_label, UI_COLOR_MUTED, 0);
    lv_label_set_long_mode(ctx->hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->hint_label, lv_pct(100));

    ctx->reset_btn = build_button(overlay, LV_SYMBOL_REFRESH " Reset counters",
                                  reset_event_cb);
    lv_obj_set_width(ctx->reset_btn, lv_pct(100));
    lv_group_add_obj(ctx->group, ctx->reset_btn);
    lv_group_focus_obj(ctx->reset_btn);

    /* Install the hook after the labels exist so the first event paints
     * against a real overlay. */
    hw_set_nfc_diag_hook(diag_hook);

    ctx->opened_tick_ms = lv_tick_get();
    render(ctx);
    /* 500 ms cadence: fast enough for the heartbeat to feel live, slow
     * enough not to churn the label tree. */
    ctx->poll_timer = lv_timer_create(poll_cb, 500, nullptr);
#endif
}
