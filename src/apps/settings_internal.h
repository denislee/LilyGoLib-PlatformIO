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

// --- Shared helpers implemented in ui_settings.cpp ---
void register_subpage_group_obj(lv_obj_t *page, lv_obj_t *obj);
lv_obj_t *create_toggle_btn_row(lv_obj_t *parent, const char *txt,
                                bool initial_state, lv_event_cb_t cb);

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
