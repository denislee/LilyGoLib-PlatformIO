/**
 * @file      settings_weather.cpp
 * @brief     Settings » Weather subpage. Extracted from ui_settings.cpp; see
 *            settings_internal.h for the cross-TU contract.
 */
#include "../ui_define.h"
#include "../ui_list_picker.h"
#include "../core/scoped_lock.h"
#include "settings_internal.h"
#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace weather_cfg {

static lv_obj_t *g_sub_page = nullptr;
static lv_obj_t *g_city_label = nullptr;
static lv_obj_t *g_status_label = nullptr;
static std::vector<weather_city_match> g_search_results;

void set_sub_page(lv_obj_t *page) { g_sub_page = page; }

#ifdef ARDUINO
static void city_search_teardown();   // defined with the async block below
#endif

void reset_state()
{
    g_sub_page = nullptr;
    g_city_label = nullptr;
    g_status_label = nullptr;
    g_search_results.clear();
#ifdef ARDUINO
    city_search_teardown();
#endif
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

// Open the city picker over the current g_search_results (which city_picked_cb
// indexes into). Shared by the async drain and the emulator path.
static void open_city_picker()
{
    std::vector<std::string> labels;
    labels.reserve(g_search_results.size());
    for (const auto &m : g_search_results) labels.push_back(m.label);
    set_status("", UI_COLOR_MUTED);
    ui_list_picker_open("Pick a city", labels, city_picked_cb, nullptr);
}

#ifdef ARDUINO
// §2.17: the geocoding lookup is a blocking HTTPS call; running it inline froze
// the LVGL thread (dead back button) for the request duration. Run it on a
// one-shot worker and deliver the result via a drain timer — same lifecycle
// contract as the Telegram-favorites fetch: the worker touches only heap state
// under the instance lock (never the widget tree), and city_search_teardown()
// kills the drain before the subpage widgets are freed. lv_timer callbacks run
// under the instance mutex (lvgl_task.cpp), so the drain needs no extra lock.
struct CitySearchResult {
    std::vector<weather_city_match> matches;
    bool ok = false;
    std::string err;
};
static TaskHandle_t      g_search_task   = nullptr;   // single worker slot
static lv_timer_t       *g_search_timer  = nullptr;
static volatile bool     g_search_done   = false;
static CitySearchResult *g_search_result = nullptr;   // heap; UI thread frees
static std::string       g_search_query;              // handed to the worker

static void city_search_task(void *arg)
{
    (void)arg;
    std::string q;
    { core::ScopedInstanceLock lock; q = g_search_query; }
    CitySearchResult *res = new CitySearchResult();
    res->ok = weather_search_cities(q.c_str(), res->matches, res->err);
    {
        core::ScopedInstanceLock lock;
        if (g_search_result) { delete g_search_result; g_search_result = nullptr; }
        g_search_result = res;
        g_search_done = true;
        g_search_task = nullptr;
    }
    vTaskDelete(NULL);
}

static void city_search_drain_tick(lv_timer_t *t)
{
    (void)t;
    if (!g_search_done) return;
    g_search_done = false;
    CitySearchResult *res = g_search_result;
    g_search_result = nullptr;
    if (g_search_timer) { lv_timer_del(g_search_timer); g_search_timer = nullptr; }
    if (!res) return;
    if (!res->ok) {
        set_status(("Search failed: " + (res->err.empty() ? std::string("err") : res->err)).c_str(),
                   lv_palette_main(LV_PALETTE_RED));
        delete res;
        return;
    }
    g_search_results = res->matches;       // city_picked_cb indexes into this
    delete res;
    open_city_picker();
}

static void city_search_teardown()
{
    if (g_search_timer) { lv_timer_del(g_search_timer); g_search_timer = nullptr; }
    if (g_search_done) {
        g_search_done = false;
        if (g_search_result) { delete g_search_result; g_search_result = nullptr; }
    }
}
#endif // ARDUINO

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
#ifdef ARDUINO
    // Geocode off-thread so the back button stays live; city_search_drain_tick
    // opens the picker (or shows the error) once the worker finishes.
    bool spawn;
    {
        core::ScopedInstanceLock lock;
        spawn = (g_search_task == nullptr);   // else a prior search is still running
        if (spawn) {
            g_search_query = q;
            g_search_done = false;
            if (g_search_result) { delete g_search_result; g_search_result = nullptr; }
        }
    }
    if (!spawn) {
        // A prior search is still in flight (e.g. the page was exited and
        // re-entered mid-fetch, which tore down the old drain timer). Re-attach
        // the drain so that worker's result still lands rather than stranding
        // it; its picker reflects the earlier query. Mirrors ui_weather.cpp.
        if (!g_search_timer)
            g_search_timer = lv_timer_create(city_search_drain_tick, 100, nullptr);
        set_status("Still searching...", UI_COLOR_ACCENT);
        return;
    }
    if (xTaskCreate(city_search_task, "wx_city", 8192, nullptr, 2,
                    &g_search_task) != pdPASS) {
        g_search_task = nullptr;
        set_status("task spawn fail", lv_palette_main(LV_PALETTE_RED));
        return;
    }
    if (!g_search_timer)
        g_search_timer = lv_timer_create(city_search_drain_tick, 100, nullptr);
#else
    // Emulator: no worker/HTTPS — resolve synchronously (the stub returns fast).
    lv_refr_now(NULL);
    std::string err;
    g_search_results.clear();
    if (!weather_search_cities(q.c_str(), g_search_results, err)) {
        set_status(("Search failed: " + (err.empty() ? std::string("err") : err)).c_str(),
                   lv_palette_main(LV_PALETTE_RED));
        return;
    }
    open_city_picker();
#endif
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
