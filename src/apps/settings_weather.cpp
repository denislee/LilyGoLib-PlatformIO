/**
 * @file      settings_weather.cpp
 * @brief     Settings » Weather subpage. Extracted from ui_settings.cpp; see
 *            settings_internal.h for the cross-TU contract.
 */
#include "../ui_define.h"
#include "../ui_list_picker.h"
#include "settings_internal.h"

namespace weather_cfg {

static lv_obj_t *g_sub_page = nullptr;
static lv_obj_t *g_city_label = nullptr;
static lv_obj_t *g_status_label = nullptr;
static std::vector<weather_city_match> g_search_results;

void set_sub_page(lv_obj_t *page) { g_sub_page = page; }

void reset_state()
{
    g_sub_page = nullptr;
    g_city_label = nullptr;
    g_status_label = nullptr;
    g_search_results.clear();
}

static void refresh_label()
{
    if (!g_city_label) return;
    std::string city = weather_get_user_city();
    lv_label_set_text_fmt(g_city_label, "City: %s",
                          city.empty() ? "(auto from IP)" : city.c_str());
}

static void set_status(const char *text, lv_color_t color)
{
    if (!g_status_label) return;
    lv_label_set_text(g_status_label, text ? text : "");
    lv_obj_set_style_text_color(g_status_label, color, 0);
}

// User picked one of the geocoding results. Persist the exact name + lat/lon
// so the Weather app's first fetch skips the geocoding round-trip entirely.
static void city_picked_cb(int index, void *ud)
{
    (void)ud;
    if (index < 0 || (size_t)index >= g_search_results.size()) {
        set_status("Cancelled", UI_COLOR_MUTED);
        return;
    }
    const weather_city_match &m = g_search_results[(size_t)index];
    weather_set_user_location(m.name.c_str(), m.lat, m.lon);
    refresh_label();
    set_status("Saved", lv_palette_main(LV_PALETTE_GREEN));
    g_search_results.clear();
}

// Text-prompt OK: query the open-meteo geocoding API for matches, then open
// a modal list of the server-accepted names. We never store a city that the
// API didn't return, so the forecast fetch can't fail on a typoed name.
static void search_entered_cb(const char *text, void *ud)
{
    (void)ud;
    if (!text) { set_status("Cancelled", UI_COLOR_MUTED); return; }
    std::string s(text);
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    std::string q = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    if (q.empty()) {
        // Empty submission clears the override and returns to auto.
        weather_set_user_city("");
        refresh_label();
        set_status("Using auto (IP)", UI_COLOR_MUTED);
        return;
    }

    set_status("Searching...", UI_COLOR_ACCENT);
    lv_refr_now(NULL);

    std::string err;
    g_search_results.clear();
    if (!weather_search_cities(q.c_str(), g_search_results, err)) {
        set_status(("Search failed: " + (err.empty() ? std::string("err") : err)).c_str(),
                   lv_palette_main(LV_PALETTE_RED));
        return;
    }

    std::vector<std::string> labels;
    labels.reserve(g_search_results.size());
    for (const auto &m : g_search_results) labels.push_back(m.label);
    set_status("", UI_COLOR_MUTED);
    ui_list_picker_open("Pick a city", labels, city_picked_cb, nullptr);
}

static void btn_set_city_cb(lv_event_t *e)
{
    (void)e;
    std::string current = weather_get_user_city();
    ui_text_prompt("Weather city",
                   "Search for a city (empty = auto).",
                   current.c_str(), search_entered_cb, nullptr);
}

static void btn_clear_city_cb(lv_event_t *e)
{
    (void)e;
    weather_set_user_city("");
    refresh_label();
    set_status("Using auto (IP)", UI_COLOR_MUTED);
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    g_city_label = lv_label_create(status);
    lv_obj_set_style_text_color(g_city_label, UI_COLOR_MUTED, 0);
    refresh_label();

    lv_obj_t *b1 = create_button(sub_page, LV_SYMBOL_KEYBOARD,
                                 "Set city", btn_set_city_cb);
    register_subpage_group_obj(sub_page, b1);

    lv_obj_t *b2 = create_button(sub_page, LV_SYMBOL_REFRESH,
                                 "Use auto (IP)", btn_clear_city_cb);
    register_subpage_group_obj(sub_page, b2);

    // Status line for search feedback ("Searching...", errors, "Saved").
    g_status_label = lv_label_create(sub_page);
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_status_label, LV_PCT(100));
    lv_label_set_text(g_status_label, "");
    lv_obj_set_style_text_color(g_status_label, UI_COLOR_MUTED, 0);
}

} // namespace weather_cfg
