/**
 * @file      menu_app.cpp
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#include "menu_app.h"
#include "../core/system.h"
#include "../ui_define.h"
#include "app_registry.h"
#include <vector>

LV_FONT_DECLARE(lv_font_montserrat_12);

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
    ITEM_REMOTE,
    ITEM_TELEGRAM,
    ITEM_NEWS,
    ITEM_SETTINGS,
};

// Notes tile now opens a launcher that contains both the text editor and the
// chronological Journal view — the standalone Journal tile has been folded in.
// File browsing lives inside Settings to keep the home grid focused on
// day-to-day apps.
static const HomeItem kItems[] = {
    {"Notes",    LV_SYMBOL_EDIT,      "Notes",    LV_PALETTE_ORANGE,      nullptr},
    {"Tasks",    LV_SYMBOL_OK,        "Tasks",    LV_PALETTE_GREEN,       nullptr},
    {"Recorder", LV_SYMBOL_AUDIO,     "Recorder", LV_PALETTE_PURPLE,      nullptr},
    {"Weather",  LV_SYMBOL_TINT,      "Weather",  LV_PALETTE_CYAN,        nullptr},
    {"Remote",   LV_SYMBOL_BLUETOOTH, "Remote",   LV_PALETTE_INDIGO,      nullptr},
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
    // Kill the badge timer before the tiles are cleaned up — otherwise its
    // next tick would touch labels that lv_obj_clean just destroyed.
    if (s_badge_timer) { lv_timer_del(s_badge_timer); s_badge_timer = nullptr; }
    s_badge_labels.clear();
    s_badge_fns.clear();
    core::App::onStop();
}

void MenuApp::onStart(lv_obj_t* parent) {
    // Turn the menu panel itself into the flex grid — avoids an extra layer and
    // makes sizing/padding straightforward.
    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_style_pad_column(parent, 6, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Reset badge tracking: tiles are about to be recreated, old pointers
    // are now invalid. onStop() kills the timer before the tiles go away.
    s_badge_labels.clear();
    s_badge_fns.clear();
    if (s_badge_timer) { lv_timer_del(s_badge_timer); s_badge_timer = nullptr; }

    lv_obj_update_layout(parent);
    int32_t panel_w = lv_obj_get_content_width(parent);
    int32_t panel_h = lv_obj_get_content_height(parent);
    if (panel_w <= 0) panel_w = 460;
    if (panel_h <= 0) panel_h = 180;

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
    lv_group_t* grp = lv_group_get_default();

    // Promote Remote to the first slot when a BLE HID host is already paired —
    // otherwise keep it in its default position right after Journal.
    int order[kItemCount];
    if (hw_get_ble_kb_connected()) {
        order[0] = ITEM_REMOTE;
        int w = 1;
        for (int i = 0; i < kItemCount; i++) {
            if (i == ITEM_REMOTE) continue;
            order[w++] = i;
        }
    } else {
        for (int i = 0; i < kItemCount; i++) order[i] = i;
    }

    for (int oi = 0; oi < kItemCount; oi++) {
        const HomeItem& item = kItems[order[oi]];
        lv_color_t accent = lv_palette_main(item.palette);

        lv_obj_t* tile = lv_btn_create(parent);
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

    if (grp && lv_obj_get_child_count(parent) > 0) {
        lv_group_focus_obj(lv_obj_get_child(parent, 0));
    }

    // Populate badges immediately, then refresh on a slow tick so the menu
    // reflects unread counts that arrive while it's open.
    if (!s_badge_labels.empty()) {
        update_badges(nullptr);
        s_badge_timer = lv_timer_create(update_badges, 2000, nullptr);
    }
}

} // namespace apps
