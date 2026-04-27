/**
 * @file      settings_internal.h
 * @brief     Private header shared between ui_settings.cpp and the split-out
 *            Settings sub-config files (settings_weather.cpp,
 *            settings_telegram.cpp, settings_notes_sync.cpp).
 *
 * Not a public API — do NOT include from anywhere else. The settings app is
 * conceptually one module; the split exists only to keep ui_settings.cpp
 * from tipping past 3,000 lines.
 *
 * Contracts:
 *  - `register_subpage_group_obj` and `create_toggle_btn_row` live in
 *    ui_settings.cpp with external linkage so each split-out TU can wire
 *    its buttons into the shared focus-group tracking.
 *  - Each sub-config namespace exposes `build_subpage()` (entry point stored
 *    on the LVGL page's user_data), `set_sub_page()` (called by
 *    create_subpage_*), and `reset_state()` (called from ui_sys_exit to
 *    clear cached LVGL pointers before the menu is deleted).
 *  - The `weather_*` and `timezone_*` forward declarations refer to symbols
 *    defined in ui_weather.cpp / ui_time_sync.cpp — those modules predate
 *    the split and still own the storage and geocoding logic.
 */
#pragma once

#include <lvgl.h>
#include <string>
#include <vector>

#include "../hal_interface.h"  // user_setting_params_t

// --- Shared helpers implemented in ui_settings.cpp ---
void register_subpage_group_obj(lv_obj_t *page, lv_obj_t *obj);
lv_obj_t *create_toggle_btn_row(lv_obj_t *parent, const char *txt,
                                bool initial_state, lv_event_cb_t cb);
// Re-populate the menu_g nav group with the subpage's registered widgets
// (skipping hidden ones). Used by subpages that toggle row visibility and
// need the keyboard focus list rebuilt in-place (connectivity's WiFi/NFC
// Test rows follow their toggle's state).
void activate_subpage_group(lv_obj_t *page);
// Event handler that maps the encoder/keyboard scroll keys (w/s, +/-, etc.)
// onto spinbox/slider increment/decrement. Attached via LV_EVENT_KEY |
// LV_EVENT_PREPROCESS. Used by datetime, backlight, and charger subpages.
void invert_scroll_key_cb(lv_event_t *e);
// Pop the Settings menu back to the root page (e.g. datetime "Save" does
// this after applying the entered time).
void settings_return_to_main_page();
// Focus-indication hook used by the Storage subpage's custom (non-
// create_toggle_btn_row) buttons: paints the parent row as LV_STATE_FOCUSED
// when the inner button gets focus, so the full row reads as highlighted.
void toggle_child_focus_cb(lv_event_t *e);
// Drop any subpage_items entries for `page` from the focus-group tracking.
// Notes Security calls this when rebuilding its page in place after a
// passphrase op, so activate_subpage_group won't re-add freed widgets.
void unregister_subpage_items_for(lv_obj_t *page);

// Working copy of the user settings blob — populated by ui_sys_enter from
// NVS and written back by ui_sys_exit. Split-out subpages mutate fields on
// this struct and call `hw_set_user_setting(local_param)` to persist
// eagerly, so settings survive a crash/power-loss between changes.
extern user_setting_params_t local_param;

// --- Timezone: defined in ui_time_sync.cpp ---
std::string timezone_get_user_tz();
void        timezone_set_user_tz(const char *tz);
bool        timezone_fetch_list(std::vector<std::string> &out, std::string &err);
bool        timezone_fetch_offset(const char *tz,
                                  int &raw_offset_sec,
                                  int &dst_offset_sec,
                                  std::string &err);

// --- Weather geocoding: defined in ui_weather.cpp ---
struct weather_city_match {
    std::string label;
    std::string name;
    double lat;
    double lon;
};
std::string weather_get_user_city();
void        weather_set_user_city(const char *city);
void        weather_set_user_location(const char *city, double lat, double lon);
bool        weather_search_cities(const char *query,
                                  std::vector<weather_city_match> &out,
                                  std::string &err);

// --- Per-sub-config entry points ---
namespace weather_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void set_sub_page(lv_obj_t *page);
    void reset_state();
}

// Local Hub subpage — master toggle + URL for the lilyhub server. Backed by
// hal::hub_*. Cross-cutting: weather and notes-sync both consult this.
namespace hub_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void set_sub_page(lv_obj_t *page);
    void reset_state();
}

namespace telegram_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void set_sub_page(lv_obj_t *page);
    void reset_state();
}

namespace notes_sync_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void set_sub_page(lv_obj_t *page);
    void reset_state();
}

// Fonts subpage has no persistent state of its own — all 14 callbacks mutate
// `local_param.*_font_index/size` fields and commit. No set_sub_page/
// reset_state needed.
namespace fonts_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
}

// Datetime subpage has its own state (spinbox pointers + NTP sync timer).
// reset_state() must run from ui_sys_exit so the poll timer doesn't outlive
// the page it drives and touch a deleted status label.
namespace datetime_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

// System Info subpage — periodic 1 Hz label refresh (clock, RSSI, storage).
// reset_state() kills the lv_timer so it doesn't tick after the page is
// gone.
namespace info_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

// Connectivity subpage — WiFi/BT/Radio/NFC/GPS/Speaker/Haptic toggles +
// "WiFi Networks", "Test Internet", "NFC Test" buttons (whose visibility
// follows the corresponding toggle's state). reset_state() nulls cached
// widget pointers so a subsequent ui_sys_enter doesn't touch stale LVGL
// objects.
namespace connectivity_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

// Display & Backlight / Charger / Performance subpages. Bundled because
// each is small on its own and they share the same dependency surface
// (local_param + invert_scroll_key_cb + toggle rows). No cached state.
namespace display_cfg {
    void build_backlight(lv_obj_t *menu, lv_obj_t *sub_page);
    void build_otg(lv_obj_t *menu, lv_obj_t *sub_page);
    void build_performance(lv_obj_t *menu, lv_obj_t *sub_page);
}

// Storage subpage — USB MSC target, pruning toggle + manual actions.
// No cached LVGL state, so no reset_state.
namespace storage_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
}

// Notes Security subpage — encryption-at-rest for *.txt notes. Bundled
// with storage because both share the storage_loader popup triad used
// for long-running filesystem ops. g_sub_page is cached so a passphrase
// op can rebuild the page in place with the fresh Status line.
namespace notes_sec_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void set_sub_page(lv_obj_t *page);
    void reset_state();
}

// Home Apps subpage — per-tile visibility toggles for the home screen
// grid. Reads/writes through apps::home_apps_is_visible / _set_visible
// (NVS-backed). No cached LVGL state.
namespace home_apps_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
}
