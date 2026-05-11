/**
 * @file      menu_app.cpp
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 *
 * Home screen. Layout, top-to-bottom:
 *   1. Quick-toggles strip (media pills + WiFi/BT), right-aligned.
 *   2. App-tile grid, ordered by activity:
 *        Create → Communicate → Info → System.
 *
 * The grid uses a darkened resting state with a subtle accent border so tiles
 * carry their identity even when unfocused, and a tinted fill + outer accent
 * halo on focus so the selected tile reads at a glance from across the room.
 */
#include "menu_app.h"
#include "../core/system.h"
#include "../ui_define.h"
#include "../hal/wireless.h"
#include "../hal/hub.h"
#include "app_registry.h"
#include <vector>
#ifdef ARDUINO
#include <Preferences.h>
#endif

LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_18);

namespace apps {

namespace {

// ---- App tiles -----------------------------------------------------------

// A badge function returns the small unread/notification count to render in
// the tile corner. Return 0 to hide the badge. Null means the tile never has
// a badge.
using BadgeFn = int (*)();

struct HomeItem {
    const char* label;
    const char* symbol;
    const char* appName;
    lv_palette_t palette;
    BadgeFn badge_fn;
    bool requires_wifi;
};

// Order groups apps by what the user is doing:
//   [create/capture]        Notes, Tasks, Recorder
//   [communicate]           Telegram, Chat
//   [consume/check]         Weather
// Settings lives in the top toggle strip as a small icon rather than the grid.
//
// Palettes are chosen so adjacent tiles never share a hue: Notes AMBER (warm
// create), Tasks GREEN (progress), Recorder PURPLE (voice), Telegram
// LIGHT_BLUE (messaging), Chat PINK (LLM Q&A — distinct from messaging blue),
// Weather CYAN (sky).
static const HomeItem kItems[] = {
    {"Notes",    LV_SYMBOL_EDIT,      "Editor",   LV_PALETTE_AMBER,       nullptr,              false},
    {"Tasks",    LV_SYMBOL_OK,        "Tasks",    LV_PALETTE_GREEN,       nullptr,              false},
    {"Recorder", LV_SYMBOL_AUDIO,     "Recorder", LV_PALETTE_PURPLE,      nullptr,              false},
    {"Telegram", LV_SYMBOL_ENVELOPE,  "Telegram", LV_PALETTE_LIGHT_BLUE,  tg_get_unread_count,  true },
    {"Chat",     LV_SYMBOL_BELL,      "Chat",     LV_PALETTE_PINK,        nullptr,              true },
    {"Weather",  LV_SYMBOL_TINT,      "Weather",  LV_PALETTE_CYAN,        nullptr,              false},
    {"SSH",      LV_SYMBOL_KEYBOARD,  "SSH",      LV_PALETTE_TEAL,        nullptr,              true },
};
constexpr int kItemCount = sizeof(kItems) / sizeof(kItems[0]);

// Keeps the badge labels alive for the lifetime of the menu view so the
// periodic timer can refresh them. Reset each time the menu re-mounts.
static std::vector<lv_obj_t*> s_badge_labels;
static std::vector<BadgeFn>   s_badge_fns;
static lv_timer_t* s_badge_timer = nullptr;

// Tiles whose backing app needs an active WiFi connection — greyed out and
// click-blocked when WiFi is off or not connected. Tracked separately from
// the full tile list so the live tick only touches the ones that change.
struct WifiGatedTile {
    lv_obj_t* tile;
    lv_obj_t* icon;
    lv_color_t accent;
};
static std::vector<WifiGatedTile> s_wifi_gated;
static int s_wifi_gated_last_state = -1;  // -1 = not yet applied

static ui_loading_t s_sync_loader;
static lv_timer_t*  s_sync_timer = nullptr;
static uint32_t     s_sync_deadline_ms = 0;

static void sync_poll_tick(lv_timer_t *t) {
    (void)t;
    int status = hw_get_time_sync_status();
    if (status == 1) {
        lv_timer_del(s_sync_timer);
        s_sync_timer = nullptr;
        ui_loading_close(&s_sync_loader);
        ui_msg_pop_up("Time Sync", "Time synchronized successfully!");
    } else if (lv_tick_get() > s_sync_deadline_ms) {
        lv_timer_del(s_sync_timer);
        s_sync_timer = nullptr;
        ui_loading_close(&s_sync_loader);
        ui_msg_pop_up("Time Sync", "Time sync timed out.");
    }
}

static void format_badge_text(int n, char *buf, size_t cap) {
    if (n > 99) snprintf(buf, cap, "99+");
    else        snprintf(buf, cap, "%d", n);
}

static void update_badges(lv_timer_t *t) {
    (void)t;
    for (size_t i = 0; i < s_badge_labels.size(); i++) {
        lv_obj_t *lbl = s_badge_labels[i];
        if (!lbl) continue;
        int n = s_badge_fns[i] ? s_badge_fns[i]() : 0;
        if (n > 0) {
            char buf[8];
            format_badge_text(n, buf, sizeof(buf));
            lv_label_set_text(lbl, buf);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Pick a scalable icon font that comfortably fits the tile height while still
// leaving room for a label below it. Thresholds tuned for the three supported
// panels (240px, 192px, 380px-tall menu areas). Only uses sizes enabled in
// every build environment (including the native emulator).
const lv_font_t* pick_icon_font(int32_t tile_h) {
    if (tile_h >= 140) return &lv_font_montserrat_48;
    if (tile_h >= 90)  return &lv_font_montserrat_32;
    if (tile_h >= 70)  return &lv_font_montserrat_28;
    if (tile_h >= 55)  return &lv_font_montserrat_24;
    return &lv_font_montserrat_20;
}

// Resting background for tiles and pills — a hair above pure black so the
// accent border stays visible on OLED/AMOLED panels without bleeding into the
// page background.
static inline lv_color_t tile_rest_bg()   { return lv_color_hex(0x15171d); }
static inline lv_color_t pill_rest_bg()   { return lv_color_hex(0x15171d); }

// ---- Quick toggles (WiFi / BT) -------------------------------------------

struct QuickToggle {
    const char* icon;
    bool (*getter)();
    void (*setter)(bool);
};

static const QuickToggle kQuickToggles[] = {
    { LV_SYMBOL_WIFI,      hw_get_wifi_enable, hw_set_wifi_enable },
    { LV_SYMBOL_BLUETOOTH, hw_get_bt_enable,   hw_set_bt_enable   },
};
constexpr int kQuickToggleCount = sizeof(kQuickToggles) / sizeof(kQuickToggles[0]);

static void apply_toggle_style(lv_obj_t* btn, bool on) {
    if (on) {
        lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_FG, 0);
    } else {
        lv_obj_set_style_bg_color(btn, pill_rest_bg(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_MUTED, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_MUTED, 0);
    }
}

static void quick_toggle_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const QuickToggle* spec = (const QuickToggle*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    if (!spec || !btn) return;
    bool new_state = !spec->getter();
    spec->setter(new_state);
    apply_toggle_style(btn, new_state);
    hw_feedback();
}

static void settings_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hw_feedback();
    core::System::getInstance().hideMenu();
    core::AppManager::getInstance().switchApp("Settings",
        core::System::getInstance().getAppPanel());
}

// ---- Internet check pill -------------------------------------------------
// Quick TCP probe to 1.1.1.1:53 (mirrors Settings → Connectivity → Test
// Internet). Pill briefly tints accent while probing — ping blocks the LV
// thread for up to 3 s so the colour change has to be flushed with
// lv_refr_now() to be visible — then a result popup reports rtt or error.

// Runs a TCP probe to 1.1.1.1:53 and shows a result popup. Optional `btn`
// is tinted accent while the probe is in flight (ping blocks the LV thread
// for up to 3 s, so we lv_refr_now() to flush the colour change first).
static void run_internet_check(lv_obj_t* btn) {
    if (!hw_get_wifi_connected()) {
        ui_result_show("Internet", "Wi-Fi is not connected.", nullptr, 0);
        return;
    }

    if (btn) {
        apply_toggle_style(btn, true);
        lv_refr_now(NULL);
    }

    uint32_t rtt_ms = 0;
    std::string err;
    bool ok = hw_ping_internet("1.1.1.1", 53, 3000, &rtt_ms, &err);

    if (btn) apply_toggle_style(btn, false);

    if (ok) {
        char rtt_buf[16];
        snprintf(rtt_buf, sizeof(rtt_buf), "%u ms", (unsigned)rtt_ms);
        ui_summary_row_t rows[] = {
            { "Host", "1.1.1.1:53" },
            { "RTT",  rtt_buf      },
        };
        ui_result_show("Internet " LV_SYMBOL_OK, "Online",
                       rows, sizeof(rows) / sizeof(rows[0]));
    } else {
        ui_result_show("Internet " LV_SYMBOL_CLOSE,
                       err.empty() ? "Failed" : err.c_str(),
                       nullptr, 0);
    }
}

static void internet_check_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hw_feedback();
    run_internet_check((lv_obj_t*)lv_event_get_target(e));
}

// ---- Glance overlay ------------------------------------------------------
// Full-screen at-a-glance readout laid out as a bento grid of cards (clock,
// date, battery, connectivity, telegram). The whole content stack is rotated
// 180° around its centre so the user can read it with the device upside-
// down — the same pose that triggers the overlay via the IMU. Lives on
// lv_layer_top so it floats above the menu without tearing it down. Any
// key press or tap dismisses it back to the main menu.

static lv_obj_t  *s_glance_overlay = nullptr;
static lv_group_t *s_glance_group  = nullptr;
static lv_group_t *s_glance_prev_group = nullptr;
static lv_obj_t  *s_glance_time_lbl = nullptr;
static lv_obj_t  *s_glance_date_lbl = nullptr;
static lv_obj_t  *s_glance_batt_lbl = nullptr;
static lv_obj_t  *s_glance_conn_lbl = nullptr;
static lv_obj_t  *s_glance_tg_lbl = nullptr;     // Telegram unread count
static lv_obj_t  *s_glance_tg_card = nullptr;    // wrapper, hidden at 0 unread
static lv_timer_t *s_glance_timer = nullptr;

static void glance_refresh(lv_timer_t *t) {
    (void)t;
    if (!s_glance_overlay) return;

    struct tm timeinfo;
    hw_get_date_time(timeinfo);
    if (s_glance_time_lbl) {
        lv_label_set_text_fmt(s_glance_time_lbl, "%02d:%02d",
                              timeinfo.tm_hour, timeinfo.tm_min);
    }
    if (s_glance_date_lbl) {
        static const char *kDays[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char *kMonths[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec"};
        int wd = timeinfo.tm_wday; if (wd < 0 || wd > 6) wd = 0;
        int mo = timeinfo.tm_mon;  if (mo < 0 || mo > 11) mo = 0;
        lv_label_set_text_fmt(s_glance_date_lbl, "%s, %s %d %d",
                              kDays[wd], kMonths[mo], timeinfo.tm_mday,
                              timeinfo.tm_year + 1900);
    }
    if (s_glance_batt_lbl) {
        monitor_params_t params;
        hw_get_monitor_params(params);
        const char *sym = LV_SYMBOL_BATTERY_FULL;
        if (params.is_charging) sym = LV_SYMBOL_CHARGE;
        else if (params.battery_percent < 20) sym = LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_glance_batt_lbl, "%s  %d%%",
                              sym, params.battery_percent);
    }
    if (s_glance_conn_lbl) {
        // Build a single line of currently-active connectivity glyphs so the
        // user can tell whether radios are up without leaving the glance.
        char buf[64];
        size_t off = 0;
        auto append = [&](const char *s) {
            if (!s) return;
            size_t l = strlen(s);
            if (off + l + 2 >= sizeof(buf)) return;
            if (off > 0) { buf[off++] = ' '; buf[off++] = ' '; }
            memcpy(buf + off, s, l);
            off += l;
        };
        if (hw_get_wifi_connected())     append(LV_SYMBOL_WIFI);
        if (hw_get_bt_enable() && hw_get_ble_kb_connected())
                                         append(LV_SYMBOL_BLUETOOTH);
        if (hal::hub_is_enabled())       append(LV_SYMBOL_HOME);
        buf[off] = 0;
        lv_label_set_text(s_glance_conn_lbl, buf);
    }
    if (s_glance_tg_lbl) {
        // Telegram unread count. Card stays visible at 0 so the date/tg
        // pair keeps a balanced 50/50 split on row 2.
        int unread = apps::tg_get_unread_count();
        lv_label_set_text_fmt(s_glance_tg_lbl,
                              LV_SYMBOL_ENVELOPE "  %d", unread);
        lv_obj_set_style_text_color(s_glance_tg_lbl,
                                    unread > 0 ? UI_COLOR_FG : UI_COLOR_MUTED, 0);
    }
}

static void glance_dismiss();

static void glance_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    // Any tap (on release) or any key on the overlay returns to the main
    // menu. Don't react to LV_EVENT_PRESSED — it fires on press-down, which
    // would dismiss before release and let the click leak through to the
    // menu underneath.
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_KEY) {
        glance_dismiss();
    }
}

static void glance_dismiss() {
    if (!s_glance_overlay) return;
    if (s_glance_timer) {
        lv_timer_del(s_glance_timer);
        s_glance_timer = nullptr;
    }
    lv_obj_del(s_glance_overlay);
    s_glance_overlay = nullptr;
    s_glance_tg_lbl = nullptr;
    s_glance_tg_card = nullptr;
    s_glance_time_lbl = nullptr;
    s_glance_date_lbl = nullptr;
    s_glance_batt_lbl = nullptr;
    s_glance_conn_lbl = nullptr;

    // Restore the menu's input group so encoder/keyboard nav resumes.
    if (s_glance_prev_group) {
        set_default_group(s_glance_prev_group);
        s_glance_prev_group = nullptr;
    }
    if (s_glance_group) {
        lv_group_del(s_glance_group);
        s_glance_group = nullptr;
    }
}

// Build a bento "card" container — rounded dark panel with centred flex
// content. Used for every cell in the glance grid so the visual language is
// uniform regardless of which payload sits inside.
static lv_obj_t *glance_make_card(lv_obj_t *parent) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_pad_row(c, 2, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return c;
}

// Transparent flex wrapper — used for the row containers inside the bento
// so the cards line up without an extra panel showing through. Caller sets
// the row's flex_grow share of the parent height; cards inside each row use
// height lv_pct(100) so they all match the row (and therefore each other).
static lv_obj_t *glance_make_row(lv_obj_t *parent, lv_flex_flow_t flow) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, 0);  // height comes from flex_grow set by caller
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_row(r, 6, 0);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, flow);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static void glance_show() {
    if (s_glance_overlay) return;

    s_glance_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_glance_overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(s_glance_overlay);
    lv_obj_set_style_bg_color(s_glance_overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_glance_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_glance_overlay, 0, 0);
    lv_obj_set_style_radius(s_glance_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_glance_overlay, 0, 0);
    lv_obj_remove_flag(s_glance_overlay, LV_OBJ_FLAG_SCROLLABLE);
    // The overlay needs to be clickable so a tap on the panel dismisses it.
    lv_obj_add_flag(s_glance_overlay, LV_OBJ_FLAG_CLICKABLE);

    // Bento content container. The whole stack is rotated 180° around its
    // centre so a user holding the device upside-down reads it right-way-up.
    // With LV_FLEX_FLOW_COLUMN_REVERSE + LV_FLEX_ALIGN_START, the first child
    // pins to the bottom of the unrotated layout — which is the *top* of the
    // screen after the 180° rotation. Children are added in visual
    // top-to-bottom order: status row, info row, then the time hero card last
    // so the clock sits at the visual *bottom* and stretches to fill the
    // remaining vertical space (flex_grow on the time card).
    lv_obj_t *content = lv_obj_create(s_glance_overlay);
    lv_obj_set_size(content, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN_REVERSE);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_transform_pivot_x(content, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(content, lv_pct(50), 0);
    lv_obj_set_style_transform_rotation(content, 1800, 0);

    // Row 1 (visual top) — battery | connectivity, side-by-side. ROW_REVERSE
    // so that post-rotation the battery card reads on the left, conn on right.
    // Each row claims a flex_grow share of the content height; cards inside
    // the row use height lv_pct(100) so the two cards in a row always share
    // the same height regardless of content.
    lv_obj_t *row_status = glance_make_row(content, LV_FLEX_FLOW_ROW_REVERSE);
    lv_obj_set_flex_grow(row_status, 1);

    lv_obj_t *batt_card = glance_make_card(row_status);
    lv_obj_set_height(batt_card, lv_pct(100));
    lv_obj_set_flex_grow(batt_card, 1);
    s_glance_batt_lbl = lv_label_create(batt_card);
    lv_obj_set_style_text_color(s_glance_batt_lbl, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_glance_batt_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_glance_batt_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *conn_card = glance_make_card(row_status);
    lv_obj_set_height(conn_card, lv_pct(100));
    lv_obj_set_flex_grow(conn_card, 1);
    s_glance_conn_lbl = lv_label_create(conn_card);
    lv_obj_set_style_text_color(s_glance_conn_lbl, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_glance_conn_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_glance_conn_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // Row 2 — date | telegram unread. Both cards stay visible so the row
    // is always a balanced 50/50 pair; the TG card just dims its label
    // when there are no unreads.
    lv_obj_t *row_info = glance_make_row(content, LV_FLEX_FLOW_ROW_REVERSE);
    lv_obj_set_flex_grow(row_info, 1);

    lv_obj_t *date_card = glance_make_card(row_info);
    lv_obj_set_height(date_card, lv_pct(100));
    lv_obj_set_flex_grow(date_card, 1);
    s_glance_date_lbl = lv_label_create(date_card);
    lv_obj_set_style_text_color(s_glance_date_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_glance_date_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_glance_date_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_glance_tg_card = glance_make_card(row_info);
    lv_obj_set_height(s_glance_tg_card, lv_pct(100));
    lv_obj_set_flex_grow(s_glance_tg_card, 1);
    s_glance_tg_lbl = lv_label_create(s_glance_tg_card);
    lv_obj_set_style_text_color(s_glance_tg_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_glance_tg_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_glance_tg_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // Row 3 (visual bottom) — TIME hero card. Wider grow share so the clock
    // dominates the screen vertically.
    lv_obj_t *time_card = glance_make_card(content);
    lv_obj_set_width(time_card, lv_pct(100));
    lv_obj_set_height(time_card, 0);
    lv_obj_set_flex_grow(time_card, 2);
    s_glance_time_lbl = lv_label_create(time_card);
    lv_obj_set_style_text_color(s_glance_time_lbl, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_glance_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(s_glance_time_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // Capture every input on the overlay so any key/tap dismisses. The
    // overlay is the only object in a private group while open, so encoder
    // rotation events also land here and trigger LV_EVENT_KEY.
    lv_obj_add_event_cb(s_glance_overlay, glance_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_glance_overlay, glance_event_cb, LV_EVENT_KEY, nullptr);

    s_glance_prev_group = lv_group_get_default();
    s_glance_group = lv_group_create();
    lv_group_set_wrap(s_glance_group, false);
    lv_group_add_obj(s_glance_group, s_glance_overlay);
    set_default_group(s_glance_group);
    lv_group_focus_obj(s_glance_overlay);

    glance_refresh(nullptr);
    s_glance_timer = lv_timer_create(glance_refresh, 1000, nullptr);
}

static void glance_click_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hw_feedback();
    glance_show();
}

// ---- Media controls (BLE HID transport keys to a paired phone) -----------
// Shown only when Bluetooth is enabled AND a BLE HID host has paired with us.
// Two buttons: play/pause (momentary) and volume. The volume button mirrors
// the Remote app — click to capture the encoder, rotate to send vol±, click
// again (or focus away) to release.
static std::vector<lv_obj_t*> s_media_buttons;  // tracked for visibility only
static lv_obj_t* s_volume_btn = nullptr;
static lv_obj_t* s_volume_icon = nullptr;
static lv_obj_t* s_wifi_btn = nullptr;  // WiFi quick-toggle pill, for 'w' shortcut
static lv_obj_t* s_bt_btn = nullptr;    // BT quick-toggle pill, for 'b' shortcut
static lv_timer_t* s_media_visibility_timer = nullptr;
static bool s_media_last_visible = false;

static bool media_controls_should_show() {
    return hw_get_bt_enable() && hw_get_ble_kb_connected();
}

static bool volume_is_editing() {
    if (!s_volume_btn) return false;
    lv_group_t* g = lv_obj_get_group(s_volume_btn);
    return g && lv_group_get_editing(g)
        && lv_group_get_focused(g) == s_volume_btn;
}

static void refresh_volume_visual() {
    if (!s_volume_btn || !s_volume_icon) return;
    bool active = volume_is_editing();
    if (active) {
        lv_obj_set_style_bg_color(s_volume_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_color(s_volume_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_opa(s_volume_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_volume_icon, UI_COLOR_FG, 0);
    } else {
        lv_obj_set_style_bg_color(s_volume_btn, pill_rest_bg(), 0);
        lv_obj_set_style_border_color(s_volume_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_opa(s_volume_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_volume_icon, UI_COLOR_ACCENT, 0);
    }
}

static void set_media_buttons_visible(bool show) {
    // The buttons live in the group from creation time (added before toggles
    // and tiles, so they sit at the front for encoder nav). Visibility is
    // purely a HIDDEN-flag toggle — lv_group_focus_next/prev already skips
    // hidden members, so we don't need to splice the group on every BT state
    // change. The old rebuild-the-group approach left the encoder indev
    // pointing at a stale focus slot when the timer fired post-boot, which
    // made the freshly-added pills unreachable until the user re-entered the
    // menu.
    lv_group_t* grp = lv_group_get_default();

    for (lv_obj_t* btn : s_media_buttons) {
        if (!btn) continue;
        if (show) {
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            // If the volume pill is mid-edit when we hide it, the encoder
            // would stay captured after it vanishes — drop editing first.
            if (btn == s_volume_btn && grp && lv_group_get_editing(grp)
                && lv_group_get_focused(grp) == s_volume_btn) {
                lv_group_set_editing(grp, false);
            }
            lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    refresh_volume_visual();
    s_media_last_visible = show;
}

static void apply_wifi_gated_state(bool connected) {
    lv_group_t* grp = lv_group_get_default();
    bool needs_focus_advance = false;

    for (const WifiGatedTile& w : s_wifi_gated) {
        if (!w.tile || !w.icon) continue;
        if (connected) {
            lv_obj_remove_state(w.tile, LV_STATE_DISABLED);
            lv_obj_set_style_border_color(w.tile, w.accent, 0);
            lv_obj_set_style_border_opa(w.tile, LV_OPA_40, 0);
            lv_obj_set_style_text_color(w.icon, w.accent, 0);
            lv_obj_set_style_opa(w.tile, LV_OPA_COVER, 0);
        } else {
            // LV_STATE_DISABLED makes lv_group_focus_next/prev skip the tile,
            // so the encoder/keyboard nav jumps straight over it.
            lv_obj_add_state(w.tile, LV_STATE_DISABLED);
            lv_obj_set_style_border_color(w.tile, UI_COLOR_MUTED, 0);
            lv_obj_set_style_border_opa(w.tile, LV_OPA_30, 0);
            lv_obj_set_style_text_color(w.icon, UI_COLOR_MUTED, 0);
            // Dim the whole tile (including label) so the disabled state reads
            // at a glance even when not focused.
            lv_obj_set_style_opa(w.tile, LV_OPA_60, 0);
            // If this tile is currently focused (user toggled WiFi off while
            // sitting on it), nudge focus forward — otherwise the encoder
            // would still register a click on a now-inert tile.
            if (grp && lv_group_get_focused(grp) == w.tile) {
                needs_focus_advance = true;
            }
        }
    }

    if (needs_focus_advance && grp) lv_group_focus_next(grp);
}

static void media_visibility_tick(lv_timer_t* t) {
    (void)t;
    bool now = media_controls_should_show();
    if (now != s_media_last_visible) {
        set_media_buttons_visible(now);
    }
    // Re-evaluate WiFi-gated tiles on the same tick — cheap, and the user
    // can flip the WiFi pill without leaving the menu.
    int wifi_now = hw_get_wifi_connected() ? 1 : 0;
    if (wifi_now != s_wifi_gated_last_state) {
        apply_wifi_gated_state(wifi_now != 0);
        s_wifi_gated_last_state = wifi_now;
    }
}

static void play_pause_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!hw_get_ble_kb_connected()) return;
    hw_feedback();
    hw_set_ble_key(MEDIA_PLAY_PAUSE);
}

// Volume button: mirrors ui_media_remote.cpp's volume_event_cb. Click toggles
// the group's editing mode so the encoder wheel stops navigating tiles and
// starts sending vol±. Click again — or focus away — releases capture.
static void volume_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    lv_group_t* g = lv_obj_get_group(btn);
    if (!g) return;

    if (code == LV_EVENT_CLICKED) {
        if (!hw_get_ble_kb_connected()) return;
        lv_group_set_editing(g, !lv_group_get_editing(g));
        hw_feedback();
        refresh_volume_visual();
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_ENTER) return;  // avoid double-toggle with CLICKED
        if (!lv_group_get_editing(g)) return;
        if (!hw_get_ble_kb_connected()) return;
        // The LilyGo encoder's clockwise detent maps to NEXT/RIGHT/UP, which
        // the Remote app sends as VOL_DOWN so wheel direction matches the
        // physical feel. Keep the same mapping here.
        if (key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_NEXT) {
            hw_feedback();
            hw_set_ble_key(MEDIA_VOLUME_DOWN);
            lv_event_stop_processing(e);
        } else if (key == LV_KEY_LEFT || key == LV_KEY_DOWN
                   || key == LV_KEY_PREV) {
            hw_feedback();
            hw_set_ble_key(MEDIA_VOLUME_UP);
            lv_event_stop_processing(e);
        }
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_group_set_editing(g, false);
        refresh_volume_visual();
    }
}

// Home-screen single-letter shortcuts. Attached to every focusable object on
// the home screen below; LV_EVENT_KEY only fires on the focused widget, so
// this is dormant elsewhere in the app.
//   d → low-power loop (real ESP lightSleep / sleep, per sleep_mode)
//   w → toggle WiFi
//   p → run internet ping test (same as the refresh pill)
//   n → open Notes (Editor)
//   t → open Tasks
static void launch_app_from_home(const char* appName) {
    hw_feedback();
    core::System::getInstance().hideMenu();
    core::AppManager::getInstance().switchApp(
        appName, core::System::getInstance().getAppPanel());
}

static void home_shortcut_key_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    uint32_t key = lv_event_get_key(e);
    if (key == 'd' || key == 'D') {
        hw_feedback();
        hw_low_power_loop();
    } else if (key == 'w' || key == 'W') {
        bool new_state = !hw_get_wifi_enable();
        hw_set_wifi_enable(new_state);
        if (s_wifi_btn) apply_toggle_style(s_wifi_btn, new_state);
        hw_feedback();
    } else if (key == 'b' || key == 'B') {
        bool new_state = !hw_get_bt_enable();
        hw_set_bt_enable(new_state);
        if (s_bt_btn) apply_toggle_style(s_bt_btn, new_state);
        hw_feedback();
    } else if (key == 'p' || key == 'P') {
        hw_feedback();
        run_internet_check(nullptr);
    } else if (key == 'n' || key == 'N') {
        launch_app_from_home("Editor");
    } else if (key == 't' || key == 'T') {
        launch_app_from_home("Tasks");
    } else if (key == 'r' || key == 'R') {
        launch_app_from_home("Recorder");
    } else if (key == 's' || key == 'S') {
        launch_app_from_home("Settings");
    } else if (key == 'z' || key == 'Z') {
        hw_feedback();
        if (hw_get_wifi_connected()) {
            if (s_sync_timer) {
                return; // Already syncing
            }
            if (hw_start_time_sync_ntp()) {
                ui_loading_open(&s_sync_loader, "Time Sync", "Syncing from internet...");
                s_sync_deadline_ms = lv_tick_get() + 20000; // 20s deadline
                s_sync_timer = lv_timer_create(sync_poll_tick, 500, nullptr);
            } else {
                ui_msg_pop_up("Time Sync", "Failed to start sync");
            }
        } else {
            ui_msg_pop_up("Time Sync", "WiFi not connected");
        }
    }
}

void tile_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const HomeItem* item = (const HomeItem*)lv_event_get_user_data(e);
    if (!item || !item->appName) return;
    // WiFi-gated tiles (e.g. Telegram) are inert when WiFi is disconnected —
    // click haptic only, no app switch. The greyed-out visual already
    // signals the disabled state, so a popup would just be noise.
    if (item->requires_wifi && !hw_get_wifi_connected()) {
        hw_feedback();
        return;
    }
    hw_feedback();
    core::System::getInstance().hideMenu();
    core::AppManager::getInstance().switchApp(item->appName,
        core::System::getInstance().getAppPanel());
}

} // namespace

MenuApp::MenuApp() : core::App("MainMenu") {}

void MenuApp::onStop() {
    // Kill timers before the tiles are cleaned up — otherwise their next
    // tick would touch labels that lv_obj_clean just destroyed.
    if (s_badge_timer) { lv_timer_del(s_badge_timer); s_badge_timer = nullptr; }
    if (s_media_visibility_timer) {
        lv_timer_del(s_media_visibility_timer);
        s_media_visibility_timer = nullptr;
    }
    s_badge_labels.clear();
    s_badge_fns.clear();
    s_media_buttons.clear();
    s_volume_btn = nullptr;
    s_volume_icon = nullptr;
    s_wifi_btn = nullptr;
    s_bt_btn = nullptr;
    s_media_last_visible = false;
    s_wifi_gated.clear();
    s_wifi_gated_last_state = -1;
    core::App::onStop();
}

void MenuApp::onStart(lv_obj_t* parent) {
    // Parent column: toggle strip on top, tile grid filling the rest.
    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_style_pad_column(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Reset mutable tracking: tiles are about to be recreated, old pointers
    // are now invalid. onStop() kills the timer before the tiles go away.
    s_badge_labels.clear();
    s_badge_fns.clear();
    if (s_badge_timer) { lv_timer_del(s_badge_timer); s_badge_timer = nullptr; }
    s_media_buttons.clear();
    s_volume_btn = nullptr;
    s_volume_icon = nullptr;
    s_wifi_btn = nullptr;
    s_bt_btn = nullptr;
    s_media_last_visible = false;
    s_wifi_gated.clear();
    s_wifi_gated_last_state = -1;
    if (s_media_visibility_timer) {
        lv_timer_del(s_media_visibility_timer);
        s_media_visibility_timer = nullptr;
    }

    lv_group_t* grp = lv_group_get_default();

    // Measure available room before carving out the toggle strip. The grid
    // gets whatever remains.
    lv_obj_update_layout(parent);
    int32_t panel_w = lv_obj_get_content_width(parent);
    int32_t panel_full_h = lv_obj_get_content_height(parent);
    if (panel_w <= 0) panel_w = 460;
    if (panel_full_h <= 0) panel_full_h = 180;

    // --- Quick-toggles strip --------------------------------------------------
    // Taller and more breathable than the first iteration so fingers can hit
    // the pills on the touch screens and the icons sit with enough margin to
    // not feel crowded against the tile grid below.
    const int32_t toggle_h = 30;
    lv_obj_t* toggle_bar = lv_obj_create(parent);
    lv_obj_set_size(toggle_bar, LV_PCT(100), toggle_h);
    lv_obj_set_style_bg_opa(toggle_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(toggle_bar, 0, 0);
    lv_obj_set_style_pad_all(toggle_bar, 0, 0);
    // Add horizontal padding so the 4px outline (2px pad + 2px width) on the outer pills
    // does not get clipped by the right/left edges of the parent container or screen.
    lv_obj_set_style_pad_right(toggle_bar, 6, 0);
    lv_obj_set_style_pad_left(toggle_bar, 6, 0);
    lv_obj_set_style_pad_column(toggle_bar, 8, 0);
    lv_obj_remove_flag(toggle_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toggle_bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(toggle_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle_bar, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto make_pill = [&](lv_obj_t* parent_bar,
                         const char* symbol,
                         int32_t w_extra) -> lv_obj_t* {
        lv_obj_t* btn = lv_btn_create(parent_bar);
        lv_obj_set_size(btn, toggle_h + w_extra, toggle_h);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(btn, toggle_h / 2, 0);
        lv_obj_set_style_bg_color(btn, pill_rest_bg(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        // Focus halo — white so it stays readable against an enabled
        // (accent-filled/bordered) pill, where an accent halo would blend in.
        lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(btn, UI_COLOR_FG, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(btn, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_color(btn, UI_COLOR_FG, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_opa(btn, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_pad(btn, 2, LV_STATE_FOCUS_KEY);

        lv_obj_t* icon = lv_label_create(btn);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
        lv_obj_center(icon);
        lv_obj_set_user_data(btn, icon);
        return btn;
    };

    // --- Media pills (play/pause + volume) ---
    // Sit just after the glance pill, created hidden; a 1 s timer shows/hides
    // them based on BLE HID pairing state.
    lv_obj_t* play_btn = make_pill(toggle_bar, LV_SYMBOL_PLAY, 14);
    lv_obj_set_style_border_color(play_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_opa(play_btn, LV_OPA_60, 0);
    lv_obj_t* play_icon = (lv_obj_t*)lv_obj_get_user_data(play_btn);
    lv_obj_set_style_text_color(play_icon, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(play_btn, play_pause_click_cb, LV_EVENT_CLICKED, nullptr);
    // Add to the group here so lv_group_focus_next/prev can skip them via the
    // HIDDEN flag while BLE is disconnected. Registering before the wifi/bt
    // toggles and app tiles keeps nav order matching the physical left-to-
    // right layout (glance pill was added first, just above).
    if (grp) lv_group_add_obj(grp, play_btn);
    s_media_buttons.push_back(play_btn);

    s_volume_btn = make_pill(toggle_bar, LV_SYMBOL_VOLUME_MAX, 14);
    lv_obj_set_style_border_color(s_volume_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_opa(s_volume_btn, LV_OPA_60, 0);
    s_volume_icon = (lv_obj_t*)lv_obj_get_user_data(s_volume_btn);
    lv_obj_set_style_text_color(s_volume_icon, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(s_volume_btn, volume_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_volume_btn, volume_event_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_volume_btn, volume_event_cb, LV_EVENT_DEFOCUSED, nullptr);
    if (grp) lv_group_add_obj(grp, s_volume_btn);
    s_media_buttons.push_back(s_volume_btn);

    set_media_buttons_visible(media_controls_should_show());

    // --- WiFi / BT toggles ---
    for (int i = 0; i < kQuickToggleCount; i++) {
        const QuickToggle& qt = kQuickToggles[i];
        lv_obj_t* btn = make_pill(toggle_bar, qt.icon, 18);
        apply_toggle_style(btn, qt.getter());
        lv_obj_add_event_cb(btn, quick_toggle_click_cb, LV_EVENT_CLICKED,
                            (void*)&qt);
        if (grp) lv_group_add_obj(grp, btn);
        if (qt.getter == hw_get_wifi_enable) s_wifi_btn = btn;
        if (qt.getter == hw_get_bt_enable) s_bt_btn = btn;
    }

    // --- Settings shortcut ---
    // Sits at the rightmost of the strip — distinct from the on/off toggles:
    // always rendered in the muted resting style, click launches the Settings
    // app directly rather than mutating a flag.
    {
        lv_obj_t* settings_btn = make_pill(toggle_bar, LV_SYMBOL_SETTINGS, 18);
        apply_toggle_style(settings_btn, false);
        lv_obj_add_event_cb(settings_btn, settings_click_cb,
                            LV_EVENT_CLICKED, nullptr);
        if (grp) lv_group_add_obj(grp, settings_btn);
    }

    // --- Tile grid container (fills the rest) -------------------------------
    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_update_layout(parent);
    int32_t grid_h = lv_obj_get_content_height(grid);
    if (grid_h <= 0) grid_h = panel_full_h - toggle_h - 6;
    if (grid_h < 50) grid_h = 50;

    // Count what's actually going on the grid first: layout has to scale to
    // the visible-tile count, not the total registry size. Without this the
    // row arithmetic below would still divide the available height by the
    // full kItemCount, leaving big empty bands when the user hides tiles.
    int visible_count = 0;
    for (int oi = 0; oi < kItemCount; oi++) {
        if (home_apps_is_visible(oi)) visible_count++;
    }

    // Empty state: user hid every tile. Drop a small hint so home isn't a
    // black void, and bail before the layout math (which would divide by 0).
    if (visible_count == 0) {
        lv_obj_t* hint = lv_label_create(grid);
        lv_label_set_text(hint, "No apps on home.\nSettings " LV_SYMBOL_RIGHT
                                " Home Apps to enable.");
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(hint, UI_COLOR_MUTED, 0);
        lv_obj_center(hint);
        // Still hand focus somewhere reachable so the encoder isn't stranded.
        if (grp && lv_obj_get_child_count(toggle_bar) > 0) {
            lv_group_focus_obj(lv_obj_get_child(toggle_bar, 0));
        }
        return;
    }

    // Pick a column count that pairs the tiles neatly:
    //   1 item    → 1 col   (full-width hero)
    //   2 items   → 2 cols
    //   4 items   → 2 cols  (2x2 reads better than 3+orphan)
    //   3 / 5+    → 3 cols  (default, matches the original layout)
    int cols;
    if (visible_count <= 1)      cols = 1;
    else if (visible_count == 2) cols = 2;
    else if (visible_count == 4) cols = 2;
    else                         cols = 3;
    int rows = (visible_count + cols - 1) / cols;
    const int gap = 8;
    int32_t tile_w = (panel_w - (cols - 1) * gap) / cols;
    int32_t tile_h = (grid_h - (rows - 1) * gap) / rows;
    if (tile_w < 50) tile_w = 50;
    if (tile_h < 50) tile_h = 50;
    // Cap tile size so a single-tile (or 2-up) layout doesn't blow into a
    // giant button. The original 3x3 sizing implicitly capped these — once
    // the grid scales down it's worth keeping the upper bound.
    const int32_t kMaxTileW = 180;
    const int32_t kMaxTileH = 140;
    if (tile_w > kMaxTileW) tile_w = kMaxTileW;
    if (tile_h > kMaxTileH) tile_h = kMaxTileH;

    const lv_font_t* icon_font = pick_icon_font(tile_h);
    const lv_font_t* label_font = get_home_font();

    for (int oi = 0; oi < kItemCount; oi++) {
        // Honor user's home-screen visibility preference. Hidden tiles are
        // skipped entirely — they don't take a grid slot and aren't in the
        // focus group, but the underlying app remains registered and can
        // still be launched from Settings/shortcuts.
        if (!home_apps_is_visible(oi)) continue;
        const HomeItem& item = kItems[oi];
        lv_color_t accent = lv_palette_main(item.palette);

        lv_obj_t* tile = lv_btn_create(grid);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        // Resting look: near-black fill with a faint colored border — the tile
        // carries its palette identity even when the user isn't on it.
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_set_style_bg_color(tile, tile_rest_bg(), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, accent, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_40, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_outline_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 6, 0);

        // Focused: palette-tinted fill, accent border at full strength, and
        // an outer accent halo so the selected tile pops from across the room.
        lv_obj_set_style_bg_color(tile, lv_palette_darken(item.palette, 4),
                                  LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(tile, accent, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(tile, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(tile, accent, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(tile, 3, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_opa(tile, LV_OPA_40, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(tile, 2, LV_STATE_FOCUSED);

        // Pressed: brief bright flash via a slightly lighter tint.
        lv_obj_set_style_bg_color(tile, lv_palette_darken(item.palette, 2),
                                  LV_STATE_PRESSED);

        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tile, 3, 0);

        lv_obj_t* icon = lv_label_create(tile);
        lv_label_set_text(icon, item.symbol);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, accent, 0);
        // On focus, lift the icon to pure white so it reads against the
        // tinted tile fill without competing for attention with the border.
        lv_obj_set_style_text_color(icon, UI_COLOR_FG, LV_STATE_FOCUSED);

        lv_obj_t* label = lv_label_create(tile);
        lv_label_set_text(label, item.label);
        lv_obj_set_style_text_color(label, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(label, label_font, 0);
        lv_obj_set_width(label, tile_w - 10);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED,
                            (void*)&item);
        if (grp) lv_group_add_obj(grp, tile);

        if (item.requires_wifi) {
            s_wifi_gated.push_back({tile, icon, accent});
        }

        // Tiles with a badge_fn get a small red pill in the top-right
        // corner, hidden by default and toggled on by update_badges().
        if (item.badge_fn) {
            lv_obj_t* badge = lv_label_create(tile);
            lv_label_set_text(badge, "");
            lv_obj_add_flag(badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -2, 2);
            lv_obj_set_style_bg_color(badge, lv_palette_main(LV_PALETTE_RED), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(badge, UI_COLOR_FG, 0);
            lv_obj_set_style_text_font(badge, &lv_font_montserrat_14, 0);
            lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_hor(badge, 5, 0);
            lv_obj_set_style_pad_ver(badge, 1, 0);
            lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
            s_badge_labels.push_back(badge);
            s_badge_fns.push_back(item.badge_fn);
        }
    }

    // Default focus to the first tile rather than a quick-toggle — tiles are
    // the primary nav target; encoder-back lands on the toggles.
    if (grp && lv_obj_get_child_count(grid) > 0) {
        lv_group_focus_obj(lv_obj_get_child(grid, 0));
    }

    // Populate badges immediately, then refresh on a slow tick so the menu
    // reflects unread counts that arrive while it's open.
    if (!s_badge_labels.empty()) {
        update_badges(nullptr);
        s_badge_timer = lv_timer_create(update_badges, 2000, nullptr);
    }

    // Apply the disabled visual to wifi-gated tiles up front so the very
    // first frame already reflects the connection state — otherwise the
    // tile flashes its accent color until the 1 s tick fires.
    if (!s_wifi_gated.empty()) {
        bool connected = hw_get_wifi_connected();
        apply_wifi_gated_state(connected);
        s_wifi_gated_last_state = connected ? 1 : 0;
    }

    // Poll BLE pairing state so media buttons appear/disappear while the
    // menu is open (user connects from their phone without leaving home),
    // and re-evaluate WiFi-gated tiles on the same tick.
    if (!s_media_buttons.empty() || !s_wifi_gated.empty()) {
        s_media_visibility_timer =
            lv_timer_create(media_visibility_tick, 1000, nullptr);
    }

    // Wire the home-screen letter shortcuts ('d'/'l'/'w'/'b'/'p'/...) on every
    // focusable object — LV_EVENT_KEY fires only on the focused widget.
    // LV_EVENT_KEY fires on the focused widget only, so attaching to each
    // group member is the way to make the shortcut work regardless of which
    // tile/pill the user happens to be sitting on.
    if (grp) {
        uint32_t n = lv_group_get_obj_count(grp);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t* o = lv_group_get_obj_by_index(grp, i);
            if (o) lv_obj_add_event_cb(o, home_shortcut_key_cb,
                                       LV_EVENT_KEY, nullptr);
        }
    }
}

// ---- Home apps visibility API -------------------------------------------
// NVS-backed (Preferences "homeapps" namespace), keyed by appName so the
// stored value survives reordering kItems. Default is "shown" — a missing
// slot returns true. The settings UI flips slots; the menu reads them on
// each rebuild (see the loop in onStart).

namespace {

// NVS keys are capped at 15 chars including the null. "v_" prefix keeps us
// well clear of the limit (longest current appName is "Recordings" → 12).
constexpr const char *kHomeAppsNs = "homeapps";

void make_vis_key(const char *appName, char out[16]) {
    snprintf(out, 16, "v_%s", appName ? appName : "");
}

} // anonymous namespace

int home_apps_count() { return kItemCount; }

const char *home_apps_label(int idx) {
    if (idx < 0 || idx >= kItemCount) return "";
    return kItems[idx].label;
}

const char *home_apps_symbol(int idx) {
    if (idx < 0 || idx >= kItemCount) return "";
    return kItems[idx].symbol;
}

bool home_apps_is_visible(int idx) {
    if (idx < 0 || idx >= kItemCount) return false;
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(kHomeAppsNs, true)) return true;
    char key[16];
    make_vis_key(kItems[idx].appName, key);
    bool v = p.getBool(key, true);
    p.end();
    return v;
#else
    return true;
#endif
}

void home_apps_set_visible(int idx, bool on) {
    if (idx < 0 || idx >= kItemCount) return;
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(kHomeAppsNs, false)) return;
    char key[16];
    make_vis_key(kItems[idx].appName, key);
    p.putBool(key, on);
    p.end();
#else
    (void)on;
#endif
}

} // namespace apps
