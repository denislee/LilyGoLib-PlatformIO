/**
 * @file      menu_app.cpp
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#include "menu_app.h"
#include "../core/system.h"
#include "../ui_define.h"
#include "../hal/wireless.h"
#include "app_registry.h"
#include <vector>

LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_18);

namespace apps {

namespace {

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
};

// Keeps the badge labels alive for the lifetime of the menu view so the
// periodic timer can refresh them. Reset each time the menu re-mounts.
static std::vector<lv_obj_t*> s_badge_labels;
static std::vector<BadgeFn>   s_badge_fns;
static lv_timer_t* s_badge_timer = nullptr;

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


enum HomeItemId {
    ITEM_NOTES,
    ITEM_TASKS,
    ITEM_RECORDER,
    ITEM_WEATHER,
    ITEM_TELEGRAM,
    ITEM_NEWS,
    ITEM_SETTINGS,
};

// Notes tile now opens a launcher that contains both the text editor and the
// chronological Journal view — the standalone Journal tile has been folded in.
// File browsing lives inside Settings to keep the home grid focused on
// day-to-day apps. The BLE media Remote is reached from Settings too — the
// quick-toggle media pills on this screen cover the common case.
static const HomeItem kItems[] = {
    {"Notes",    LV_SYMBOL_EDIT,      "Notes",    LV_PALETTE_ORANGE,      nullptr},
    {"Tasks",    LV_SYMBOL_OK,        "Tasks",    LV_PALETTE_GREEN,       nullptr},
    {"Recorder", LV_SYMBOL_AUDIO,     "Recorder", LV_PALETTE_PURPLE,      nullptr},
    {"Weather",  LV_SYMBOL_TINT,      "Weather",  LV_PALETTE_CYAN,        nullptr},
    {"Telegram", LV_SYMBOL_ENVELOPE,  "Telegram", LV_PALETTE_LIGHT_BLUE,  tg_get_unread_count},
    {"News",     LV_SYMBOL_LIST,      "News",     LV_PALETTE_BLUE,        nullptr},
    {"Settings", LV_SYMBOL_SETTINGS,  "Settings", LV_PALETTE_GREY,        nullptr},
};
constexpr int kItemCount = sizeof(kItems) / sizeof(kItems[0]);

// Pick a scalable icon font that comfortably fits the tile height while still
// leaving room for a label below it. Thresholds tuned for the three supported
// panels (240px, 192px, 380px-tall menu areas). Only uses sizes that are
// enabled across all build environments (including the native emulator).
const lv_font_t* pick_icon_font(int32_t tile_h) {
    if (tile_h >= 140) return &lv_font_montserrat_48;
    if (tile_h >= 90)  return &lv_font_montserrat_32;
    if (tile_h >= 70)  return &lv_font_montserrat_28;
    if (tile_h >= 55)  return &lv_font_montserrat_24;
    return &lv_font_montserrat_20;
}

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
        lv_obj_set_style_text_color(btn, UI_COLOR_FG, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x151515), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_MUTED, 0);
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

// --- Media controls (HID transport keys to a paired phone) ---
// Shown only when Bluetooth is enabled AND a BLE HID host has paired with us.
// Two buttons: play/pause (momentary) and volume. The volume button mirrors
// the Remote app's behaviour — click to capture the encoder, rotate to send
// vol±, click again (or focus away) to release.
static std::vector<lv_obj_t*> s_media_buttons;  // tracked for visibility only
static lv_obj_t* s_volume_btn = nullptr;
static lv_obj_t* s_volume_icon = nullptr;
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
        lv_obj_set_style_bg_color(s_volume_btn, lv_color_hex(0x151515), 0);
        lv_obj_set_style_border_color(s_volume_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_opa(s_volume_btn, LV_OPA_40, 0);
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

static void media_visibility_tick(lv_timer_t* t) {
    (void)t;
    bool now = media_controls_should_show();
    if (now != s_media_last_visible) {
        set_media_buttons_visible(now);
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

void tile_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const HomeItem* item = (const HomeItem*)lv_event_get_user_data(e);
    if (!item || !item->appName) return;
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
    s_media_last_visible = false;
    core::App::onStop();
}

void MenuApp::onStart(lv_obj_t* parent) {
    // Parent lays out a small quick-toggles row on top and the tile grid below.
    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 4, 0);
    lv_obj_set_style_pad_column(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Reset badge tracking: tiles are about to be recreated, old pointers
    // are now invalid. onStop() kills the timer before the tiles go away.
    s_badge_labels.clear();
    s_badge_fns.clear();
    if (s_badge_timer) { lv_timer_del(s_badge_timer); s_badge_timer = nullptr; }
    s_media_buttons.clear();
    s_volume_btn = nullptr;
    s_volume_icon = nullptr;
    s_media_last_visible = false;
    if (s_media_visibility_timer) {
        lv_timer_del(s_media_visibility_timer);
        s_media_visibility_timer = nullptr;
    }

    lv_group_t* grp = lv_group_get_default();

    // Measure available room on the full panel before carving out the
    // toggle bar. The grid gets whatever remains.
    lv_obj_update_layout(parent);
    int32_t panel_w = lv_obj_get_content_width(parent);
    int32_t panel_full_h = lv_obj_get_content_height(parent);
    if (panel_w <= 0) panel_w = 460;
    if (panel_full_h <= 0) panel_full_h = 180;

    // --- Quick toggles row (WiFi, Bluetooth) ---
    const int32_t toggle_h = 26;
    lv_obj_t* toggle_bar = lv_obj_create(parent);
    lv_obj_set_size(toggle_bar, LV_PCT(100), toggle_h);
    lv_obj_set_style_bg_opa(toggle_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(toggle_bar, 0, 0);
    lv_obj_set_style_pad_all(toggle_bar, 0, 0);
    lv_obj_set_style_pad_column(toggle_bar, 6, 0);
    lv_obj_remove_flag(toggle_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(toggle_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle_bar, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Media buttons sit to the left of the wifi/bt toggles. Created hidden;
    // a 1-second timer shows/hides them based on BLE HID pairing state.
    auto make_media_pill = [&](const char* symbol) -> lv_obj_t* {
        lv_obj_t* btn = lv_btn_create(toggle_bar);
        lv_obj_set_size(btn, toggle_h + 10, toggle_h);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(btn, toggle_h / 2, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x151515), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(btn, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(btn, 1, LV_STATE_FOCUSED);

        lv_obj_t* icon = lv_label_create(btn);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_color(icon, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
        lv_obj_center(icon);
        lv_obj_set_user_data(btn, icon);
        return btn;
    };

    lv_obj_t* play_btn = make_media_pill(LV_SYMBOL_PLAY);
    lv_obj_add_event_cb(play_btn, play_pause_click_cb, LV_EVENT_CLICKED, nullptr);
    // Add to the group up front so lv_group_focus_next/prev can skip them via
    // the HIDDEN flag while BLE is disconnected. Registering them here — before
    // the wifi/bt toggles and app tiles are appended — pins them at the front
    // of the group so nav order matches the physical left-to-right layout.
    if (grp) lv_group_add_obj(grp, play_btn);
    s_media_buttons.push_back(play_btn);

    s_volume_btn = make_media_pill(LV_SYMBOL_VOLUME_MAX);
    s_volume_icon = (lv_obj_t*)lv_obj_get_user_data(s_volume_btn);
    lv_obj_add_event_cb(s_volume_btn, volume_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_volume_btn, volume_event_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_volume_btn, volume_event_cb, LV_EVENT_DEFOCUSED, nullptr);
    if (grp) lv_group_add_obj(grp, s_volume_btn);
    s_media_buttons.push_back(s_volume_btn);

    set_media_buttons_visible(media_controls_should_show());

    for (int i = 0; i < kQuickToggleCount; i++) {
        const QuickToggle& qt = kQuickToggles[i];
        lv_obj_t* btn = lv_btn_create(toggle_bar);
        lv_obj_set_size(btn, toggle_h + 14, toggle_h);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(btn, toggle_h / 2, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(btn, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(btn, 1, LV_STATE_FOCUSED);

        lv_obj_t* icon = lv_label_create(btn);
        lv_label_set_text(icon, qt.icon);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
        lv_obj_center(icon);

        apply_toggle_style(btn, qt.getter());
        lv_obj_add_event_cb(btn, quick_toggle_click_cb, LV_EVENT_CLICKED,
                            (void*)&qt);
        if (grp) lv_group_add_obj(grp, btn);
    }

    // --- Tile grid container (fills the rest) ---
    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_update_layout(parent);
    int32_t panel_h = lv_obj_get_content_height(grid);
    if (panel_h <= 0) panel_h = panel_full_h - toggle_h - 4;
    if (panel_h < 50) panel_h = 50;

    // 4 cols on landscape panels (pager, ultra); 3 cols on the square watch.
    // Rows adapt to item count so added apps still fit the grid.
    int cols = (panel_w >= 400) ? 4 : 3;
    int rows = (kItemCount + cols - 1) / cols;
    const int gap = 6;
    int32_t tile_w = (panel_w - (cols - 1) * gap) / cols;
    int32_t tile_h = (panel_h - (rows - 1) * gap) / rows;
    if (tile_w < 50) tile_w = 50;
    if (tile_h < 50) tile_h = 50;

    const lv_font_t* icon_font = pick_icon_font(tile_h);
    const lv_font_t* label_font = get_home_font();

    for (int oi = 0; oi < kItemCount; oi++) {
        const HomeItem& item = kItems[oi];
        lv_color_t accent = lv_palette_main(item.palette);

        lv_obj_t* tile = lv_btn_create(grid);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        // Resting look: near-black fill with a faint colored border — the only
        // thing hinting at the app's identity at a glance.
        lv_obj_set_style_radius(tile, 12, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x151515), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, accent, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_40, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_outline_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 4, 0);

        // Focused: accent border at full strength + gentle tint. Readable on
        // all three displays without relying on color alone.
        lv_obj_set_style_border_color(tile, accent, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(tile, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(tile, lv_palette_darken(item.palette, 4),
                                  LV_STATE_FOCUSED);

        // Pressed: brief bright flash via a slightly lighter tint.
        lv_obj_set_style_bg_color(tile, lv_palette_darken(item.palette, 3),
                                  LV_STATE_PRESSED);

        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tile, 2, 0);

        lv_obj_t* icon = lv_label_create(tile);
        lv_label_set_text(icon, item.symbol);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, accent, 0);

        lv_obj_t* label = lv_label_create(tile);
        lv_label_set_text(label, item.label);
        lv_obj_set_style_text_color(label, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(label, label_font, 0);
        lv_obj_set_width(label, tile_w - 8);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED,
                            (void*)&item);
        if (grp) lv_group_add_obj(grp, tile);

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
            lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
            lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_hor(badge, 4, 0);
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

    // Poll BLE pairing state so media buttons appear/disappear while the
    // menu is open (user connects from their phone without leaving home).
    if (!s_media_buttons.empty()) {
        s_media_visibility_timer =
            lv_timer_create(media_visibility_tick, 1000, nullptr);
    }
}

} // namespace apps
