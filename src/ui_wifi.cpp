/**
 * @file      ui_wifi.cpp
 * @brief     Modal WiFi network picker: scan, connect, forget saved creds.
 *
 * Opened from the Connectivity settings subpage. Renders on lv_layer_top so
 * it fully covers the settings menu (and the status-bar back button). Uses
 * the existing passphrase prompt for password entry.
 */
#include "ui_define.h"
#include "core/input_focus.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct WifiCtx {
    lv_obj_t *overlay      = nullptr;
    lv_obj_t *status_label = nullptr;
    lv_obj_t *list         = nullptr;
    lv_obj_t *scan_btn     = nullptr;
    lv_obj_t *forget_btn   = nullptr;
    lv_timer_t *poll_timer = nullptr;
    lv_group_t *group      = nullptr;
    lv_group_t *prev_group = nullptr;
    lv_event_cb_t prev_back_cb = nullptr;
    bool scan_in_flight    = false;
    bool connect_in_flight = false;
    std::string pending_ssid;
    std::string pending_password;   // cached for save-on-success
    uint8_t pending_authmode = 0;
    uint32_t connect_deadline_ms = 0;
    // Snapshot of the SSIDs currently rendered in the list, in display order.
    // Clicks look up SSIDs here instead of re-querying the scan driver, so
    // synthesised entries (e.g. the connected network shown during a scan)
    // still resolve correctly.
    std::vector<std::string> displayed_ssids;
};

static WifiCtx *g_ctx = nullptr;

static void set_status(const char *msg)
{
    if (!g_ctx || !g_ctx->status_label) return;
    lv_label_set_text(g_ctx->status_label, msg ? msg : "");
}

static void populate_list(const std::vector<wifi_scan_params_t> &nets);

static void refresh_scan_results()
{
    std::vector<wifi_scan_params_t> nets;
    hw_get_wifi_scan_result(nets);
    populate_list(nets);
}

// Populate the list with just the currently-connected network (if any).
// Lets users see their network immediately while the scan is still in flight;
// the scan-complete handler will replace this with the full list.
static void populate_connected_only()
{
    std::vector<wifi_scan_params_t> placeholder;
    if (hw_get_wifi_connected()) {
        wifi_scan_params_t p;
        std::string ssid;
        hw_get_wifi_ssid(ssid);
        p.ssid = ssid;
        p.rssi = hw_get_wifi_rssi();
        // We don't have the real authmode until the scan completes, but
        // since we're associated it's almost certainly secured — mark it as
        // non-OPEN so the list doesn't render a misleading "open" hint.
        p.authmode = 1;
        p.channel = 0;
        placeholder.push_back(p);
    }
    populate_list(placeholder);
}

static void start_scan()
{
    if (!g_ctx || g_ctx->scan_in_flight) return;
    if (!hw_get_wifi_enable()) {
        hw_set_wifi_enable(true);
    }
    set_status("Scanning...");
    populate_connected_only();
    hw_set_wifi_scan();
    g_ctx->scan_in_flight = true;
}

static void tear_down();

static void close_event_cb(lv_event_t *e)
{
    (void)e;
    tear_down();
}

static void scan_event_cb(lv_event_t *e)
{
    (void)e;
    start_scan();
}

static void forget_event_cb(lv_event_t *e)
{
    (void)e;
    hw_wifi_forget();
    set_status("Saved network removed.");
    if (g_ctx && g_ctx->forget_btn) {
        lv_obj_add_state(g_ctx->forget_btn, LV_STATE_DISABLED);
    }
}

static void kick_off_connect(const std::string &ssid, const std::string &pw)
{
    if (!g_ctx) return;
    wifi_conn_params_t p;
    p.ssid     = ssid;
    p.password = pw;
    hw_set_wifi_connect(p);
    g_ctx->pending_ssid = ssid;
    g_ctx->pending_password = pw;
    g_ctx->connect_in_flight = true;
    g_ctx->connect_deadline_ms = lv_tick_get() + 20000; /* 20 s timeout */
    char buf[96];
    snprintf(buf, sizeof(buf), "Connecting to %s...", ssid.c_str());
    set_status(buf);
}

/* Password prompt OK path: fire connect and start poll for association. */
static void password_entered_cb(const char *pw, void *ud)
{
    (void)ud;
    if (!g_ctx) return;
    if (!pw) {
        /* User cancelled. */
        set_status("Cancelled.");
        return;
    }
    kick_off_connect(g_ctx->pending_ssid, pw);
}

static void net_clicked_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    if (!g_ctx || !btn) return;
    intptr_t idx = (intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || (size_t)idx >= g_ctx->displayed_ssids.size()) return;
    std::string ssid = g_ctx->displayed_ssids[idx];

    /* Look up the authmode from the latest scan snapshot. For entries
     * synthesised before the scan completes (e.g. the connected-network
     * placeholder) the scan list may not have the SSID yet; treat as
     * unknown/locked so the connect path still works. */
    uint8_t authmode = 1;
    {
        std::vector<wifi_scan_params_t> nets;
        hw_get_wifi_scan_result(nets);
        for (const auto &n : nets) {
            if (n.ssid == ssid) { authmode = n.authmode; break; }
        }
    }
    g_ctx->pending_ssid = ssid;
    g_ctx->pending_authmode = authmode;

    /* Tapping the currently-connected network disconnects instead of
     * re-associating. Gives users a way to drop a connection without
     * forgetting (which would wipe the saved password). */
    if (hw_get_wifi_connected()) {
        std::string cur_ssid;
        hw_get_wifi_ssid(cur_ssid);
        if (cur_ssid == ssid) {
            hw_set_wifi_disconnect();
            g_ctx->connect_in_flight = false;
            char buf[96];
            snprintf(buf, sizeof(buf), "Disconnected from %s.", ssid.c_str());
            set_status(buf);
            refresh_scan_results();
            return;
        }
    }

    /* Already a favorite — reuse the stored password so the user doesn't
     * have to re-enter it. Works for open networks too (saved password is
     * empty). */
    std::string saved_pw;
    if (hw_wifi_get_saved_password(ssid, saved_pw)) {
        kick_off_connect(ssid, saved_pw);
        return;
    }

    /* Open network — authmode 0 is WIFI_AUTH_OPEN on ESP32. */
    if (authmode == 0) {
        kick_off_connect(ssid, "");
        return;
    }

    char title[96];
    snprintf(title, sizeof(title), "Connect to %s", ssid.c_str());
    ui_passphrase_prompt(title, "Enter WiFi password", /*confirm=*/false,
                         password_entered_cb, nullptr);
}

static const char *authmode_label(uint8_t mode)
{
    /* ESP-IDF wifi_auth_mode_t mapping. Anything that's not OPEN gets a
     * lock badge; the specifics (WPA2/WPA3/etc.) aren't worth exposing. */
    return (mode == 0) ? "" : LV_SYMBOL_WARNING " ";
}

static bool is_saved_ssid(const std::string &ssid,
                          const std::vector<std::string> &saved)
{
    for (const auto &s : saved) {
        if (s == ssid) return true;
    }
    return false;
}

static void populate_list(const std::vector<wifi_scan_params_t> &nets)
{
    if (!g_ctx || !g_ctx->list) return;
    lv_obj_clean(g_ctx->list);
    g_ctx->displayed_ssids.clear();

    if (nets.empty()) {
        lv_obj_t *empty = lv_label_create(g_ctx->list);
        lv_label_set_text(empty, "No networks found");
        lv_obj_set_style_text_color(empty, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_pad_all(empty, 16, 0);
        return;
    }

    // Favorites first, preserving the scan's RSSI ordering within each group.
    std::vector<std::string> saved;
    hw_wifi_get_saved_list(saved);
    std::vector<size_t> order;
    order.reserve(nets.size());
    for (size_t i = 0; i < nets.size(); ++i) {
        if (is_saved_ssid(nets[i].ssid, saved)) order.push_back(i);
    }
    for (size_t i = 0; i < nets.size(); ++i) {
        if (!is_saved_ssid(nets[i].ssid, saved)) order.push_back(i);
    }

    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        const wifi_scan_params_t &n = nets[i];
        bool fav = is_saved_ssid(n.ssid, saved);
        char label[160];
        snprintf(label, sizeof(label), "%s%s%s  (%d dBm)",
                 fav ? LV_SYMBOL_OK " " : "",
                 authmode_label(n.authmode), n.ssid.c_str(), (int)n.rssi);
        lv_obj_t *btn = lv_list_add_btn(g_ctx->list, LV_SYMBOL_WIFI, label);
        // user_data holds the display index into g_ctx->displayed_ssids.
        lv_obj_set_user_data(btn, (void *)(intptr_t)g_ctx->displayed_ssids.size());
        g_ctx->displayed_ssids.push_back(n.ssid);
        lv_obj_add_event_cb(btn, net_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_group_add_obj(g_ctx->group, btn);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_ctx) return;
    // Pause while the password prompt is open (its textarea is focused) —
    // hw_get_wifi_scan_result does a scan-result pull over the instance
    // mutex that the keyboard reader also needs.
    if (ui_text_input_focused()) return;

    if (g_ctx->scan_in_flight && !hw_get_wifi_scanning()) {
        g_ctx->scan_in_flight = false;
        refresh_scan_results();
        std::vector<wifi_scan_params_t> nets;
        hw_get_wifi_scan_result(nets);
        char buf[64];
        snprintf(buf, sizeof(buf), "Found %u network(s).", (unsigned)nets.size());
        set_status(buf);
    }

    if (g_ctx->connect_in_flight) {
        if (hw_get_wifi_connected()) {
            g_ctx->connect_in_flight = false;
            // Save only after association succeeds so bad passwords don't
            // stick in the favorites list.
            hw_wifi_add_saved(g_ctx->pending_ssid, g_ctx->pending_password);
            std::string ip;
            hw_get_ip_address(ip);
            char buf[128];
            snprintf(buf, sizeof(buf), "Connected. IP: %s", ip.c_str());
            set_status(buf);
            if (g_ctx->forget_btn) {
                lv_obj_remove_state(g_ctx->forget_btn, LV_STATE_DISABLED);
            }
            // Refresh the list so the new favorite gets its star badge.
            refresh_scan_results();
        } else if ((int32_t)(lv_tick_get() - g_ctx->connect_deadline_ms) > 0) {
            g_ctx->connect_in_flight = false;
            set_status("Connection failed or timed out.");
        }
    }
}

static void tear_down()
{
    if (!g_ctx) return;
    WifiCtx *ctx = g_ctx;
    g_ctx = nullptr;

    if (ctx->poll_timer) {
        lv_timer_del(ctx->poll_timer);
    }
    if (ctx->overlay) {
        lv_obj_del(ctx->overlay);
    }
    if (ctx->group) {
        lv_group_del(ctx->group);
    }
    if (ctx->prev_group) {
        set_default_group(ctx->prev_group);
    }
    // Restore the previous back-button handler so the underlying screen (the
    // Settings subpage that opened us) keeps working. ui_show_back_button
    // re-adds the button to the now-restored default group.
    if (ctx->prev_back_cb) {
        ui_show_back_button(ctx->prev_back_cb);
    }
    delete ctx;
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

void ui_wifi_networks_open()
{
    if (g_ctx) return;
    WifiCtx *ctx = new WifiCtx();
    g_ctx = ctx;

    ctx->prev_group = lv_group_get_default();
    ctx->prev_back_cb = ui_get_back_button_cb();
    ctx->group = lv_group_create();
    lv_group_set_wrap(ctx->group, false);
    set_default_group(ctx->group);

    // Re-point the status-bar back button at our close handler so both touch
    // and keyboard navigation land on a working "back" inside this overlay.
    // ui_show_back_button adds the button to the current default group, which
    // we've just set to ours, so it becomes keyboard-reachable too.
    ui_show_back_button(close_event_cb);

    enable_keyboard();

    // Match the status-bar height so the overlay sits just below it instead
    // of covering it — users still see time/battery/etc. while picking a
    // network. Formula mirrors core::System::setupGlobalUI().
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
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(overlay, 6, 0);

    /* Button row: Scan, Forget. Close is handled by the status-bar back
     * button, re-pointed to close_event_cb above. */
    lv_obj_t *row = lv_obj_create(overlay);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    ctx->scan_btn   = build_button(row, LV_SYMBOL_REFRESH " Scan",  scan_event_cb);
    ctx->forget_btn = build_button(row, LV_SYMBOL_TRASH   " Forget", forget_event_cb);
    lv_obj_set_flex_grow(ctx->scan_btn,   1);
    lv_obj_set_flex_grow(ctx->forget_btn, 1);
    if (!hw_wifi_has_saved()) {
        lv_obj_add_state(ctx->forget_btn, LV_STATE_DISABLED);
    }
    lv_group_add_obj(ctx->group, ctx->scan_btn);
    lv_group_add_obj(ctx->group, ctx->forget_btn);

    /* Status line under the buttons. */
    ctx->status_label = lv_label_create(overlay);
    lv_label_set_text(ctx->status_label, "");
    lv_obj_set_style_text_color(ctx->status_label, UI_COLOR_MUTED, 0);
    lv_label_set_long_mode(ctx->status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->status_label, lv_pct(100));
    lv_obj_set_style_text_align(ctx->status_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Scrollable list of networks grows to fill the remaining space. */
    ctx->list = lv_list_create(overlay);
    lv_obj_set_width(ctx->list, lv_pct(100));
    lv_obj_set_flex_grow(ctx->list, 1);
    lv_obj_set_style_radius(ctx->list, UI_RADIUS, 0);

    lv_group_focus_obj(ctx->scan_btn);

    ctx->poll_timer = lv_timer_create(poll_cb, 500, nullptr);

    /* If the radio's already associated (e.g. auto-reconnect succeeded on
     * boot), surface that up front. */
    if (hw_get_wifi_connected()) {
        std::string ssid, ip;
        hw_get_wifi_ssid(ssid);
        hw_get_ip_address(ip);
        char buf[160];
        snprintf(buf, sizeof(buf), "Connected: %s (%s)", ssid.c_str(), ip.c_str());
        set_status(buf);
    }

    /* Kick off an initial scan so the list isn't empty on open. */
    start_scan();
}
