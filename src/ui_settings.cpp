/**
 * @file      ui_settings.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"
#include "hal/notes_crypto.h"
#include "core/app_manager.h"
#include "core/system.h"
#include "apps/app_registry.h"
#include "ui_list_picker.h"
#include <vector>

// Defined in ui_weather.cpp — read/write the user-chosen city used by the
// weather app. Empty city means "use IP-based auto location".
std::string weather_get_user_city();
void weather_set_user_city(const char *city);
void weather_set_user_location(const char *city, double lat, double lon);

// Weather geocoding search — returns up to ~10 matches the open-meteo API
// accepts. Declared inline here (matching the struct layout in ui_weather.cpp)
// so we don't need to spin up a separate header for one call site.
struct weather_city_match {
    std::string label;
    std::string name;
    double lat;
    double lon;
};
bool weather_search_cities(const char *query,
                           std::vector<weather_city_match> &out,
                           std::string &err);

// Defined in ui_time_sync.cpp — bundled IANA-city list + local offset lookup
// driven by newlib's POSIX TZ rules, used to compute an accurate GMT offset
// before NTP sync.
std::string timezone_get_user_tz();
void        timezone_set_user_tz(const char *tz);
bool        timezone_fetch_list(std::vector<std::string> &out, std::string &err);
// Returns true on success and fills raw_offset_sec + dst_offset_sec.
bool        timezone_fetch_offset(const char *tz,
                                  int &raw_offset_sec,
                                  int &dst_offset_sec,
                                  std::string &err);

static void style_menu_item_icon(lv_obj_t *cont, const char *icon, const char *text)
{
    lv_obj_t *icon_label = lv_label_create(cont);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_min_width(icon_label, 20, 0);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *text_label = lv_label_create(cont);
    lv_label_set_text(text_label, text);
}

static lv_obj_t *menu = NULL;
static lv_timer_t *timer = NULL;
static  user_setting_params_t local_param;
static uint32_t get_ip_id = 0;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *settings_main_page = NULL;
static lv_obj_t *settings_exit_btn = NULL;

void ui_sys_exit(lv_obj_t *parent);

#define MAX_MAIN_PAGE_ITEMS 16
static lv_obj_t *main_page_group_items[MAX_MAIN_PAGE_ITEMS];
static uint8_t main_page_group_count = 0;

#define MAX_SUBPAGE_ITEMS 64
static struct {
    lv_obj_t *page;
    lv_obj_t *obj;
} subpage_items[MAX_SUBPAGE_ITEMS];
static uint8_t subpage_item_count = 0;

static void add_main_page_group_item(lv_obj_t *obj)
{
    if (main_page_group_count < MAX_MAIN_PAGE_ITEMS) {
        main_page_group_items[main_page_group_count++] = obj;
    }
    lv_group_add_obj(menu_g, obj);
}

static void register_subpage_group_obj(lv_obj_t *page, lv_obj_t *obj)
{
    if (subpage_item_count < MAX_SUBPAGE_ITEMS) {
        subpage_items[subpage_item_count].page = page;
        subpage_items[subpage_item_count].obj = obj;
        subpage_item_count++;
    }
}

static void restore_main_page_group()
{
    lv_group_remove_all_objs(menu_g);
    if (settings_exit_btn) {
        lv_group_add_obj(menu_g, settings_exit_btn);
    }
    for (uint8_t i = 0; i < main_page_group_count; i++) {
        lv_group_add_obj(menu_g, main_page_group_items[i]);
    }
    if (settings_exit_btn) {
        lv_group_focus_obj(settings_exit_btn);
    }
}

static void activate_subpage_group(lv_obj_t *page)
{
    lv_group_remove_all_objs(menu_g);
    // Status bar back is the visible "<" on subpages — its callback pops to
    // root. Keep it keyboard-navigable and initially focused.
    if (settings_exit_btn) {
        lv_group_add_obj(menu_g, settings_exit_btn);
    }
    for (uint8_t i = 0; i < subpage_item_count; i++) {
        if (subpage_items[i].page == page &&
            !lv_obj_has_flag(subpage_items[i].obj, LV_OBJ_FLAG_HIDDEN)) {
            lv_group_add_obj(menu_g, subpage_items[i].obj);
        }
    }
    if (settings_exit_btn) {
        lv_group_focus_obj(settings_exit_btn);
    }
}

static lv_obj_t *create_toggle_btn_row(lv_obj_t *parent, const char *txt, bool initial_state, lv_event_cb_t cb);

typedef struct {

    lv_obj_t *datetime_label;
    lv_obj_t *wifi_rssi_label;
    lv_obj_t *batt_voltage_label;

    lv_obj_t *wifi_ssid_label;
    lv_obj_t *ip_info_label;
    lv_obj_t *sd_size_label;
    lv_obj_t *storage_used_label;
    lv_obj_t *storage_free_label;
    lv_obj_t *local_storage_total_label;
    lv_obj_t *local_storage_used_label;
    lv_obj_t *local_storage_free_label;
    bool info_loaded;

} sys_label_t;

static sys_label_t sys_label;


static void sys_timer_event_cb(lv_timer_t*t)
{
    string datetime;
    hw_get_date_time(datetime);
    lv_label_set_text_fmt(sys_label.datetime_label, "%s", datetime.c_str());

    if (hw_get_wifi_connected()) {
        lv_label_set_text_fmt(sys_label.wifi_rssi_label, "%d", hw_get_wifi_rssi());
    }
    lv_label_set_text_fmt(sys_label.batt_voltage_label, "%d mV", hw_get_battery_voltage());

    if (!sys_label.info_loaded) {
        string wifi_ssid = "N/A";
        hw_get_wifi_ssid(wifi_ssid);
        if (sys_label.wifi_ssid_label) lv_label_set_text(sys_label.wifi_ssid_label, wifi_ssid.c_str());

        string ip_info = "N/A";
        hw_get_ip_address(ip_info);
        if (sys_label.ip_info_label) lv_label_set_text(sys_label.ip_info_label, ip_info.c_str());

        uint64_t total = 0, used = 0, free = 0;
        hw_get_storage_info(total, used, free);
        char buffer[64];
        
        auto format_size = [](char *buf, size_t len, uint64_t bytes) {
            if (bytes > 1024ULL * 1024 * 1024) {
                snprintf(buf, len, "%.2f GB", bytes / 1024.0 / 1024.0 / 1024.0);
            } else if (bytes > 1024 * 1024) {
                snprintf(buf, len, "%.2f MB", bytes / 1024.0 / 1024.0);
            } else if (bytes > 1024) {
                snprintf(buf, len, "%.2f KB", bytes / 1024.0);
            } else {
                snprintf(buf, len, "%llu B", bytes);
            }
        };

        if (total > 0) {
            format_size(buffer, sizeof(buffer), total);
            if (sys_label.sd_size_label) lv_label_set_text(sys_label.sd_size_label, buffer);

            format_size(buffer, sizeof(buffer), used);
            if (sys_label.storage_used_label) lv_label_set_text(sys_label.storage_used_label, buffer);

            format_size(buffer, sizeof(buffer), free);
            if (sys_label.storage_free_label) lv_label_set_text(sys_label.storage_free_label, buffer);
        } else {
            if (sys_label.sd_size_label) lv_label_set_text(sys_label.sd_size_label, "N/A");
            if (sys_label.storage_used_label) lv_label_set_text(sys_label.storage_used_label, "N/A");
            if (sys_label.storage_free_label) lv_label_set_text(sys_label.storage_free_label, "N/A");
        }

        uint64_t l_total = 0, l_used = 0, l_free = 0;
        hw_get_local_storage_info(l_total, l_used, l_free);
        if (l_total > 0) {
            format_size(buffer, sizeof(buffer), l_total);
            if (sys_label.local_storage_total_label) lv_label_set_text(sys_label.local_storage_total_label, buffer);

            format_size(buffer, sizeof(buffer), l_used);
            if (sys_label.local_storage_used_label) lv_label_set_text(sys_label.local_storage_used_label, buffer);

            format_size(buffer, sizeof(buffer), l_free);
            if (sys_label.local_storage_free_label) lv_label_set_text(sys_label.local_storage_free_label, buffer);
        } else {
            if (sys_label.local_storage_total_label) lv_label_set_text(sys_label.local_storage_total_label, "N/A");
            if (sys_label.local_storage_used_label) lv_label_set_text(sys_label.local_storage_used_label, "N/A");
            if (sys_label.local_storage_free_label) lv_label_set_text(sys_label.local_storage_free_label, "N/A");
        }

        sys_label.info_loaded = true;
    }
}

static long map_r(long x, long in_min, long in_max, long out_min, long out_max)
{
    if (x < in_min) {
        return out_min;
    } else if (x > in_max) {
        return out_max;
    }
    return ((x - in_min) * (out_max - out_min)) / (in_max - in_min) + out_min;
}


static void invert_scroll_key_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;
    uint32_t *key = (uint32_t *)lv_event_get_param(e);
    if (!key) return;
    switch (*key) {
    case LV_KEY_LEFT:  *key = LV_KEY_RIGHT; break;
    case LV_KEY_RIGHT: *key = LV_KEY_LEFT;  break;
    }
}

static void display_brightness_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t val =  lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);

    uint16_t min_brightness = hw_get_disp_min_brightness();
    uint16_t max_brightness = hw_get_disp_max_brightness();

    lv_label_set_text_fmt(slider_label, "   %u%%  ", map_r(val, min_brightness, max_brightness, 0, 100));
    local_param.brightness_level = val;
    hw_set_disp_backlight(val);
}

static void keyboard_brightness_cb(lv_event_t *e)
{
    // Slider is 0..20 so each encoder tick moves 5% of full range.
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    int32_t step = lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);
    uint8_t val = (uint8_t)map_r(step, 0, 20, 0, 255);
    lv_label_set_text_fmt(slider_label, "   %ld%%  ", step * 5);
    local_param.keyboard_bl_level = val;
    hw_set_kb_backlight(val);
}

static void led_brightness_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t val =  lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);
    lv_label_set_text_fmt(slider_label, "   %u%%  ", map_r(val, 0, 255, 0, 100));
    local_param.led_indicator_level = val;
    hw_set_led_backlight(val);
}


static void disp_timeout_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t val =  lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);

    local_param.disp_timeout_second = val;
    if (val == 0) {
        lv_label_set_text(slider_label, " Always ");
    } else {
        lv_label_set_text_fmt(slider_label, "   %uS  ", val);
    }
}

static void sleep_mode_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t val = (uint8_t)lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);

    local_param.sleep_mode = val;
    if (val == 0) {
        lv_label_set_text(slider_label, " Light ");
    } else {
        lv_label_set_text(slider_label, " Deep ");
    }
}

static void otg_output_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
        lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);
        printf("State: %s\n", turnOn ? "On" : "Off");
        if (hw_set_otg(turnOn) == false) {
            lv_obj_remove_state(obj, LV_STATE_CHECKED);
            if (slider_label) lv_label_set_text(slider_label, " Off ");
        } else {
            if (slider_label) lv_label_set_text(slider_label, turnOn ? " On " : " Off ");
        }
    }
}

static void charger_enable_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
        lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);
        local_param.charger_enable = turnOn;
        printf("State: %s\n", turnOn ? "On" : "Off");
        
        // Immediately save the setting so the background loop respects it
        hw_set_user_setting(local_param);
        
        hw_set_charger(turnOn);
        
        // Refresh battery logic to apply any limits based on new setting
        hw_update_battery_history();
        
        if (slider_label) lv_label_set_text(slider_label, turnOn ? " On " : " Off ");
    }
}

static void charger_current_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(slider);
    int32_t val = lv_slider_get_value(slider);
    local_param.charger_current = hw_set_charger_current_level(val );
    lv_label_set_text_fmt(slider_label, "%04umA", local_param.charger_current);
    
    // Save the new current immediately
    hw_set_user_setting(local_param);
}

static void charge_limit_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(slider);
    bool turnOn = lv_obj_has_state(slider, LV_STATE_CHECKED);
    local_param.charge_limit_en = turnOn;
    
    // Immediately save the setting so the background loop picks it up
    hw_set_user_setting(local_param);

    // If we turned off the limit, restore the default charger state
    if (!turnOn) {
        hw_set_charger(local_param.charger_enable);
    } else {
        // If we turned on the limit, refresh battery logic to apply it immediately
        hw_update_battery_history();
    }
    
    if (slider_label) lv_label_set_text(slider_label, turnOn ? " On " : " Off ");
}

static void show_mem_usage_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
        lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
        local_param.show_mem_usage = turnOn;
        // Save immediately for status bar to update
        hw_set_user_setting(local_param);
        if (label) lv_label_set_text(label, turnOn ? " On " : " Off ");
    }
}

static void show_file_count_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
        lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
        local_param.show_file_count = turnOn;
        hw_set_user_setting(local_param);
        if (label) lv_label_set_text(label, turnOn ? " On " : " Off ");
    }
}

static void spinbox_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *sb = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = lv_obj_get_group(sb);

    if (code == LV_EVENT_CLICKED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev && lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) {
            bool editing = lv_group_get_editing(g);
            lv_group_set_editing(g, !editing);
        }
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        lv_indev_t *indev = lv_indev_get_act();
        bool editing = lv_group_get_editing(g);

        if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER && key == LV_KEY_ENTER) {
            lv_group_set_editing(g, !editing);
            lv_event_stop_processing(e);
            return;
        }

        if (key == 'a' || key == 'A') lv_spinbox_step_prev(sb);
        else if (key == 'd' || key == 'D') lv_spinbox_step_next(sb);
        else if (key == 'w' || key == 'W' || key == '+') lv_spinbox_increment(sb);
        else if (key == 's' || key == 'S' || key == '-') lv_spinbox_decrement(sb);
        else if (key == 'q' || key == 'Q' || key == LV_KEY_UP) {
            lv_group_focus_prev(g);
        }
        else if (key == 'e' || key == 'E' || key == LV_KEY_DOWN) {
            lv_group_focus_next(g);
        }
    }
}

typedef struct {
    lv_obj_t *year;
    lv_obj_t *mon;
    lv_obj_t *day;
    lv_obj_t *hour;
    lv_obj_t *min;
    lv_obj_t *sync_status;   // status label next to "Sync from Internet"
    lv_obj_t *tz_label;      // shows the currently-selected IANA timezone
    lv_timer_t *sync_timer;  // poll timer while a sync is in progress
    uint32_t sync_deadline_ms;
} datetime_setup_t;

static datetime_setup_t dt_setup;

// worldtimeapi timezone list held between the "Set timezone" button and the
// picker callback — cleared once the user picks one (or cancels).
static std::vector<std::string> g_tz_candidates;

static void refresh_tz_label()
{
    if (!dt_setup.tz_label) return;
    std::string tz = timezone_get_user_tz();
    lv_label_set_text(dt_setup.tz_label,
                      tz.empty() ? "(device default)" : tz.c_str());
}

static void save_datetime_cb(lv_event_t *e)
{
    datetime_setup_t *setup_ptr = (datetime_setup_t *)lv_event_get_user_data(e);
    struct tm timeinfo = {0};

    timeinfo.tm_year = lv_spinbox_get_value(setup_ptr->year) - 1900;
    timeinfo.tm_mon = lv_spinbox_get_value(setup_ptr->mon) - 1;
    timeinfo.tm_mday = lv_spinbox_get_value(setup_ptr->day);
    timeinfo.tm_hour = lv_spinbox_get_value(setup_ptr->hour);
    timeinfo.tm_min = lv_spinbox_get_value(setup_ptr->min);
    timeinfo.tm_sec = 0;

    hw_set_date_time(timeinfo);

    lv_menu_clear_history(menu);
    lv_menu_set_page(menu, settings_main_page);
}

// --- NTP sync ------------------------------------------------------------
// Pushes the fresh RTC time back into the spinboxes so the user sees the
// new value without having to leave and re-enter the page.
static void refresh_datetime_spinboxes()
{
    struct tm timeinfo;
    hw_get_date_time(timeinfo);
    if (dt_setup.year) lv_spinbox_set_value(dt_setup.year, timeinfo.tm_year + 1900);
    if (dt_setup.mon)  lv_spinbox_set_value(dt_setup.mon,  timeinfo.tm_mon + 1);
    if (dt_setup.day)  lv_spinbox_set_value(dt_setup.day,  timeinfo.tm_mday);
    if (dt_setup.hour) lv_spinbox_set_value(dt_setup.hour, timeinfo.tm_hour);
    if (dt_setup.min)  lv_spinbox_set_value(dt_setup.min,  timeinfo.tm_min);
}

static void sync_set_status(const char *text, lv_color_t color)
{
    if (!dt_setup.sync_status) return;
    lv_label_set_text(dt_setup.sync_status, text ? text : "");
    lv_obj_set_style_text_color(dt_setup.sync_status, color, 0);
}

static void sync_stop_timer()
{
    if (dt_setup.sync_timer) {
        lv_timer_del(dt_setup.sync_timer);
        dt_setup.sync_timer = nullptr;
    }
}

// Polls the SNTP status ~3×/s. SNTP completion triggers factory.ino's
// time_available callback which writes the RTC, so here we only need to
// pick up the result and refresh the UI. A 15s ceiling covers slow DNS on
// first-time use without blocking the subpage indefinitely.
static void sync_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (hw_get_time_sync_status() == 1) {
        sync_stop_timer();
        refresh_datetime_spinboxes();
        sync_set_status(LV_SYMBOL_OK " Synced", lv_palette_main(LV_PALETTE_GREEN));
        return;
    }
    if (lv_tick_get() > dt_setup.sync_deadline_ms) {
        sync_stop_timer();
        sync_set_status("Timed out", lv_palette_main(LV_PALETTE_RED));
    }
}

static void sync_time_cb(lv_event_t *)
{
    sync_stop_timer();
    if (!hw_get_wifi_connected()) {
        sync_set_status("WiFi not connected", lv_palette_main(LV_PALETTE_RED));
        return;
    }

    // If the user has picked a city, resolve its current offset via
    // worldtimeapi first so the wall-clock time that lands in the RTC is
    // correct for that city (including DST). A failure here is non-fatal:
    // we fall back to the compile-time GMT_OFFSET_SECOND so sync still
    // completes instead of leaving the user with no clock fix.
    bool use_tz_offset = false;
    int offset_sec = 0;
    std::string tz = timezone_get_user_tz();
    if (!tz.empty()) {
        sync_set_status("Resolving timezone...", UI_COLOR_ACCENT);
        lv_refr_now(NULL);
        int raw = 0, dst = 0;
        std::string err;
        if (timezone_fetch_offset(tz.c_str(), raw, dst, err)) {
            offset_sec = raw + dst;
            use_tz_offset = true;
        } else {
            sync_set_status(("TZ lookup failed: " + err).c_str(),
                            lv_palette_main(LV_PALETTE_RED));
            // Don't return — NTP sync is still valuable with the default offset.
        }
    }

    bool started = use_tz_offset
        ? hw_start_time_sync_ntp(offset_sec)
        : hw_start_time_sync_ntp();
    if (!started) {
        sync_set_status("Sync failed to start", lv_palette_main(LV_PALETTE_RED));
        return;
    }
    sync_set_status("Syncing...", UI_COLOR_ACCENT);
    dt_setup.sync_deadline_ms = lv_tick_get() + 15000;
    dt_setup.sync_timer = lv_timer_create(sync_poll_cb, 300, nullptr);
}

// --- Timezone picker -----------------------------------------------------

static void tz_picked_cb(int index, void *ud)
{
    (void)ud;
    if (index < 0 || (size_t)index >= g_tz_candidates.size()) {
        g_tz_candidates.clear();
        return;
    }
    timezone_set_user_tz(g_tz_candidates[(size_t)index].c_str());
    refresh_tz_label();
    g_tz_candidates.clear();
    sync_set_status("Timezone saved. Tap Sync to apply.", UI_COLOR_MUTED);
}

static void set_tz_cb(lv_event_t *)
{
    g_tz_candidates.clear();
    std::string err;
    if (!timezone_fetch_list(g_tz_candidates, err)) {
        sync_set_status(("List failed: " + err).c_str(),
                        lv_palette_main(LV_PALETTE_RED));
        return;
    }
    sync_set_status("", UI_COLOR_MUTED);
    ui_list_picker_open("Pick a timezone", g_tz_candidates, tz_picked_cb, nullptr);
}

static void clear_tz_cb(lv_event_t *)
{
    timezone_set_user_tz("");
    refresh_tz_label();
    sync_set_status("Using device default offset", UI_COLOR_MUTED);
}

static void build_subpage_datetime(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_set_flex_flow(sub_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sub_page, 6, 0);
    lv_obj_set_style_pad_row(sub_page, 6, 0);

    // Fresh page build → drop any pointers left over from a previous visit
    // so sync_set_status/refresh don't touch stale LVGL objects.
    sync_stop_timer();
    dt_setup.sync_status = nullptr;
    dt_setup.tz_label    = nullptr;

    auto add_section_header = [&](const char *text) {
        lv_obj_t *l = lv_label_create(sub_page);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_pad_top(l, 4, 0);
        lv_obj_set_style_pad_bottom(l, 0, 0);
    };

    auto add_card = [&]() {
        lv_obj_t *c = lv_obj_create(sub_page);
        lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, 6, 0);
        lv_obj_set_style_pad_row(c, 6, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        return c;
    };

    auto add_row = [&](lv_obj_t *parent, lv_flex_align_t main) {
        lv_obj_t *r = lv_obj_create(parent);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 6, 0);
        return r;
    };

    struct tm timeinfo;
    hw_get_date_time(timeinfo);

    auto create_sb = [&](lv_obj_t *parent, const char *title, int min, int max, int val, int digits) {
        lv_obj_t *col = lv_obj_create(parent);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 2, 0);

        lv_obj_t *l = lv_label_create(col);
        lv_label_set_text(l, title);
        lv_obj_set_style_text_color(l, UI_COLOR_MUTED, 0);

        lv_obj_t *sb = lv_spinbox_create(col);
        lv_spinbox_set_range(sb, min, max);
        lv_spinbox_set_digit_format(sb, digits, 0);
        lv_spinbox_set_value(sb, val);
        lv_obj_set_width(sb, digits == 4 ? 70 : 50);
        lv_obj_add_event_cb(sb, spinbox_event_cb, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(sb, invert_scroll_key_cb,
                            (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);
        register_subpage_group_obj(sub_page, sb);
        return sb;
    };

    /* ============ Section 1: Set manually ============ */
    add_section_header("Set manually");
    lv_obj_t *manual_card = add_card();

    lv_obj_t *date_row = add_row(manual_card, LV_FLEX_ALIGN_SPACE_EVENLY);
    dt_setup.year = create_sb(date_row, "Year", 2000, 2099, timeinfo.tm_year + 1900, 4);
    dt_setup.mon  = create_sb(date_row, "Mon",  1, 12, timeinfo.tm_mon + 1, 2);
    dt_setup.day  = create_sb(date_row, "Day",  1, 31, timeinfo.tm_mday, 2);

    lv_obj_t *time_row = add_row(manual_card, LV_FLEX_ALIGN_CENTER);
    dt_setup.hour = create_sb(time_row, "Hour", 0, 23, timeinfo.tm_hour, 2);
    lv_obj_t *colon = lv_label_create(time_row);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_24, 0);
    dt_setup.min  = create_sb(time_row, "Min", 0, 59, timeinfo.tm_min, 2);

    lv_obj_t *save_btn = lv_btn_create(manual_card);
    lv_obj_set_width(save_btn, LV_PCT(100));
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, LV_SYMBOL_OK "  Apply");
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save_btn, save_datetime_cb, LV_EVENT_CLICKED, &dt_setup);
    register_subpage_group_obj(sub_page, save_btn);

    /* ============ Section 2: Timezone ============
     * Stored in NVS; "Use default" clears the override so the device falls
     * back to the compile-time GMT_OFFSET_SECOND. */
    add_section_header("Timezone");
    lv_obj_t *tz_card = add_card();

    dt_setup.tz_label = lv_label_create(tz_card);
    lv_label_set_long_mode(dt_setup.tz_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(dt_setup.tz_label, LV_PCT(100));
    refresh_tz_label();

    lv_obj_t *tz_btn_row = add_row(tz_card, LV_FLEX_ALIGN_SPACE_BETWEEN);

    lv_obj_t *tz_btn = lv_btn_create(tz_btn_row);
    lv_obj_set_flex_grow(tz_btn, 1);
    lv_obj_t *tz_btn_label = lv_label_create(tz_btn);
    lv_label_set_text(tz_btn_label, LV_SYMBOL_GPS "  Pick");
    lv_obj_center(tz_btn_label);
    lv_obj_add_event_cb(tz_btn, set_tz_cb, LV_EVENT_CLICKED, nullptr);
    register_subpage_group_obj(sub_page, tz_btn);

    lv_obj_t *tz_clear_btn = lv_btn_create(tz_btn_row);
    lv_obj_set_flex_grow(tz_clear_btn, 1);
    lv_obj_t *tz_clear_label = lv_label_create(tz_clear_btn);
    lv_label_set_text(tz_clear_label, LV_SYMBOL_CLOSE "  Default");
    lv_obj_center(tz_clear_label);
    lv_obj_add_event_cb(tz_clear_btn, clear_tz_cb, LV_EVENT_CLICKED, nullptr);
    register_subpage_group_obj(sub_page, tz_clear_btn);

    /* ============ Section 3: Network sync ============ */
    add_section_header("Network sync");
    lv_obj_t *sync_card = add_card();

    lv_obj_t *sync_btn = lv_btn_create(sync_card);
    lv_obj_set_width(sync_btn, LV_PCT(100));
    lv_obj_t *sync_label = lv_label_create(sync_btn);
    lv_label_set_text(sync_label, LV_SYMBOL_WIFI "  Sync from Internet");
    lv_obj_center(sync_label);
    lv_obj_add_event_cb(sync_btn, sync_time_cb, LV_EVENT_CLICKED, nullptr);
    register_subpage_group_obj(sub_page, sync_btn);

    // Status line grows in place below the button so the card doesn't jump.
    dt_setup.sync_status = lv_label_create(sync_card);
    lv_label_set_long_mode(dt_setup.sync_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(dt_setup.sync_status, LV_PCT(100));
    lv_label_set_text(dt_setup.sync_status, "");
    lv_obj_set_style_text_color(dt_setup.sync_status, UI_COLOR_MUTED, 0);
}

static lv_obj_t *create_subpage_datetime(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_REFRESH, "Date & Time");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_datetime);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static void build_subpage_backlight(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    lv_obj_t *slider;
    lv_obj_t *parent;
    lv_obj_t *slider_label;

    uint16_t min_brightness = hw_get_disp_min_brightness();
    uint16_t max_brightness = hw_get_disp_max_brightness();

    auto add_slider = [&](const char *txt, int32_t min, int32_t max, int32_t val,
                          lv_event_cb_t cb, const char *fmt_str) {
        slider = create_slider(sub_page, NULL, txt, min, max, val, cb, LV_EVENT_VALUE_CHANGED);
        parent = lv_obj_get_parent(slider);
        slider_label = lv_label_create(parent);
        char buf[16];
        snprintf(buf, sizeof(buf), fmt_str, (int)map_r(val, min, max, 0, 100));
        lv_label_set_text(slider_label, buf);
        lv_obj_set_user_data(slider, slider_label);
        register_subpage_group_obj(sub_page, slider);
    };

    add_slider("Screen", min_brightness, max_brightness,
               local_param.brightness_level, display_brightness_cb, "%d%%");
    lv_obj_add_event_cb(slider, invert_scroll_key_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);

    if (hw_has_keyboard()) {
        // 0..20 slider range = 21 discrete 5% steps; each encoder tick moves 5%.
        int32_t kb_step = (int32_t)map_r(local_param.keyboard_bl_level, 0, 255, 0, 20);
        add_slider("Keyboard", 0, 20, kb_step, keyboard_brightness_cb, "%d%%");
        lv_obj_add_event_cb(slider, invert_scroll_key_cb,
                            (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);
    }

    if (hw_has_indicator_led()) {
        add_slider("LED", 0, 255,
                   local_param.led_indicator_level, led_brightness_cb, "%d%%");
    }

    slider = create_slider(sub_page, NULL, "Timeout", 0, 180,
                           local_param.disp_timeout_second, disp_timeout_cb, LV_EVENT_VALUE_CHANGED);
    parent = lv_obj_get_parent(slider);
    slider_label = lv_label_create(parent);
    if (local_param.disp_timeout_second == 0) {
        lv_label_set_text(slider_label, " Always ");
    } else {
        lv_label_set_text_fmt(slider_label, "   %uS  ", local_param.disp_timeout_second);
    }
    lv_obj_set_user_data(slider, slider_label);
    register_subpage_group_obj(sub_page, slider);
    lv_obj_add_event_cb(slider, invert_scroll_key_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);

    slider = create_slider(sub_page, NULL, "Sleep Mode", 0, 1,
                           local_param.sleep_mode, sleep_mode_cb, LV_EVENT_VALUE_CHANGED);
    parent = lv_obj_get_parent(slider);
    slider_label = lv_label_create(parent);
    if (local_param.sleep_mode == 0) {
        lv_label_set_text(slider_label, " Light ");
    } else {
        lv_label_set_text(slider_label, " Deep ");
    }
    lv_obj_set_user_data(slider, slider_label);
    register_subpage_group_obj(sub_page, slider);

    lv_obj_t *btn = create_toggle_btn_row(sub_page, "Show Memory", local_param.show_mem_usage, show_mem_usage_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "Show File Count", local_param.show_file_count, show_file_count_cb);
    register_subpage_group_obj(sub_page, btn);
}

static lv_obj_t *create_subpage_backlight(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_IMAGE, "Display & Backlight");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_backlight);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static void build_subpage_otg(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    bool enableOtg = hw_get_otg_enable();
    uint8_t total_charge_level = hw_get_charge_level_nums();
    uint8_t curr_charge_level = hw_get_charger_current_level();

    lv_obj_t *btn;

    if (hw_has_otg_function()) {
        btn = create_toggle_btn_row(sub_page, "OTG Output", enableOtg, otg_output_cb);
        register_subpage_group_obj(sub_page, btn);
    }

    btn = create_toggle_btn_row(sub_page, "Charging", local_param.charger_enable, charger_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    lv_obj_t *slider = create_slider(sub_page, NULL, "Current",
                                     1, total_charge_level, curr_charge_level,
                                     charger_current_cb, LV_EVENT_VALUE_CHANGED);
    lv_obj_t *parent = lv_obj_get_parent(slider);
    lv_obj_t *slider_label = lv_label_create(parent);
    lv_label_set_text_fmt(slider_label, "%umA", local_param.charger_current);
    lv_obj_set_user_data(slider, slider_label);
    register_subpage_group_obj(sub_page, slider);
    lv_obj_add_event_cb(slider, invert_scroll_key_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);

    btn = create_toggle_btn_row(sub_page, "Limit 80%", local_param.charge_limit_en, charge_limit_cb);
    register_subpage_group_obj(sub_page, btn);
}

static lv_obj_t *create_subpage_otg(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_CHARGE, "Charger");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_otg);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static void cpu_freq_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint16_t index = lv_dropdown_get_selected(obj);
    uint16_t freqs[] = {240, 160, 80};
    local_param.cpu_freq_mhz = freqs[index];
    
    // Save immediately so the loop picks it up
    hw_set_user_setting(local_param);
}

static void build_subpage_performance(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    const char *freq_options = "240 MHz (Max)\n160 MHz\n80 MHz (Power)";
    uint16_t current_freq = local_param.cpu_freq_mhz;
    uint8_t sel_idx = 0; // Default 240
    if (current_freq == 160) sel_idx = 1;
    else if (current_freq == 80) sel_idx = 2;

    lv_obj_t *dd = create_dropdown(sub_page, NULL, "CPU Clock", freq_options, sel_idx, cpu_freq_cb);

    // Size the dropdown wide enough to comfortably show the longest
    // option ("240 MHz (Max)") plus the arrow, but not stretched across
    // the whole row. Title label absorbs the remaining width.
    lv_obj_t *row = lv_obj_get_parent(dd);
    lv_obj_t *title_label = lv_obj_get_child(row, 0);
    lv_obj_set_flex_grow(title_label, 1);
    lv_obj_set_width(title_label, 0);
    lv_obj_set_flex_grow(dd, 0);
    lv_obj_set_width(dd, LV_PCT(45));

    register_subpage_group_obj(sub_page, dd);
}

static lv_obj_t *create_subpage_performance(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_SETTINGS, "Performance");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_performance);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static void build_subpage_info(lv_obj_t *menu, lv_obj_t *sub_page)
{
    sys_label.info_loaded = false;
    lv_obj_add_flag(sub_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sub_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sub_page, 4, 0);
    lv_obj_set_style_pad_row(sub_page, 0, 0);

    auto add_info_row = [&](const char *key, const char *val) -> lv_obj_t* {
        lv_obj_t *row = lv_menu_cont_create(sub_page);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
        lv_obj_set_style_radius(row, 0, 0);

        lv_obj_t *k = lv_label_create(row);
        lv_label_set_text(k, key);
        lv_obj_set_style_text_color(k, lv_palette_main(LV_PALETTE_GREY), 0);

        lv_obj_t *v = lv_label_create(row);
        lv_label_set_text(v, val);
        lv_label_set_long_mode(v, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_max_width(v, LV_PCT(55), 0);

        register_subpage_group_obj(sub_page, row);
        return v;
    };

    char buffer[128];
    uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    bool has_mac = hw_get_mac(mac);
    if (has_mac) {
        snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    add_info_row("MAC", has_mac ? buffer : "N/A");

    sys_label.wifi_ssid_label = add_info_row("WiFi", "Loading...");
    sys_label.datetime_label = add_info_row("RTC", "00:00:00");
    sys_label.ip_info_label = add_info_row("IP", "Loading...");
    sys_label.wifi_rssi_label = add_info_row("RSSI", "N/A");

    snprintf(buffer, sizeof(buffer), "%d mV", hw_get_battery_voltage());
    sys_label.batt_voltage_label = add_info_row("Battery", buffer);

#if defined(HAS_SD_CARD_SOCKET)
    const char *storage_name = "SD Total";
#else
    const char *storage_name = "Storage Total";
#endif
    sys_label.sd_size_label = add_info_row(storage_name, "Loading...");
    sys_label.storage_used_label = add_info_row("Storage Used", "Loading...");
    sys_label.storage_free_label = add_info_row("Storage Free", "Loading...");

    sys_label.local_storage_total_label = add_info_row("Internal Total", "Loading...");
    sys_label.local_storage_used_label = add_info_row("Internal Used", "Loading...");
    sys_label.local_storage_free_label = add_info_row("Internal Free", "Loading...");

    snprintf(buffer, sizeof(buffer), "%d.%d.%d", lv_version_major(), lv_version_minor(), lv_version_patch());
    add_info_row("LVGL", buffer);

    string ver;
    hw_get_arduino_version(ver);
    add_info_row("Core", ver.c_str());

    add_info_row("Built", __DATE__);
    add_info_row("Hash", hw_get_firmware_hash_string());
    add_info_row("Chip", hw_get_chip_id_string());

    timer = lv_timer_create(sys_timer_event_cb, 1000, NULL);
}

static lv_obj_t *create_subpage_info(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_LIST, "System Info");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_info);
    lv_menu_set_load_page_event(menu, cont, sub_page);

    return cont;
}

static void build_device_probe(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_add_flag(sub_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sub_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sub_page, 4, 0);
    lv_obj_set_style_pad_row(sub_page, 0, 0);

    uint8_t devices = hw_get_devices_nums();
    uint32_t devices_mask = hw_get_device_online();
    for (int i = 0; i < devices; ++i) {
        const char *device_name = hw_get_devices_name(i);
        if (lv_strcmp(device_name, "") != 0) {
            bool online = (devices_mask & 0x01);

            lv_obj_t *row = lv_menu_cont_create(sub_page);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_ver(row, 5, 0);
            lv_obj_set_style_pad_hor(row, 8, 0);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(row, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);

            lv_obj_t *name = lv_label_create(row);
            lv_label_set_text(name, device_name);
            lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL);
            lv_obj_set_style_max_width(name, LV_PCT(65), 0);

            lv_obj_t *status = lv_label_create(row);
            if (online) {
                lv_label_set_text(status, LV_SYMBOL_OK);
                lv_obj_set_style_text_color(status, lv_palette_main(LV_PALETTE_GREEN), 0);
            } else {
                lv_label_set_text(status, LV_SYMBOL_CLOSE);
                lv_obj_set_style_text_color(status, lv_palette_main(LV_PALETTE_RED), 0);
            }

            register_subpage_group_obj(sub_page, row);
        }
        devices_mask >>= 1;
    }
}

static lv_obj_t *create_device_probe(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_USB, "Devices");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_device_probe);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static void files_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hw_feedback();
    // Defer the app switch: ui_sys_exit() deletes the settings `menu`, which
    // is an ancestor of the Files menu item whose CLICKED event is still
    // being dispatched. Freeing it synchronously leaves LVGL walking freed
    // memory and freezes the device. queueSwitchApp runs from
    // AppManager::update() after lv_timer_handler fully unwinds — guaranteed
    // next-main-loop-iteration, unlike lv_async_call (period-0 timer) which
    // races the current event dispatch and could miss the first click.
    core::AppManager::getInstance().queueSwitchApp("Files",
        core::System::getInstance().getAppPanel());
}

static lv_obj_t *create_files_item(lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    style_menu_item_icon(cont, LV_SYMBOL_DIRECTORY, "Files");
    lv_obj_add_event_cb(cont, files_click_cb, LV_EVENT_CLICKED, NULL);
    return cont;
}

static void power_off_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hw_feedback();
    lv_delay_ms(200);
    hw_shutdown();
}

static lv_obj_t *create_power_off_item(lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    style_menu_item_icon(cont, LV_SYMBOL_POWER, "Power Off");
    lv_obj_t *icon_label = lv_obj_get_child(cont, 0);
    if (icon_label) {
        lv_obj_set_style_text_color(icon_label,
                                    lv_palette_main(LV_PALETTE_RED), 0);
    }
    lv_obj_add_event_cb(cont, power_off_click_cb, LV_EVENT_CLICKED, NULL);
    return cont;
}


typedef void (*subpage_builder_t)(lv_obj_t *menu, lv_obj_t *page);

static void settings_page_changed_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    // Ignore events bubbling from children (like dropdowns)
    if (lv_event_get_target(e) != obj) return;

    lv_obj_t *page = lv_menu_get_cur_main_page(obj);
    if (page != settings_main_page) {
        subpage_builder_t builder = (subpage_builder_t)lv_obj_get_user_data(page);
        if (builder) {
            builder(menu, page);
            lv_obj_set_user_data(page, NULL); // Only build once
        }
        activate_subpage_group(page);
    } else {
        restore_main_page_group();
    }
}

static void settings_exit_cb(lv_event_t *e)
{
    // On the root settings page the status bar back exits the app entirely.
    // On a subpage it pops back to the root page. Forwarding a click to the
    // built-in back button proved unreliable (the header back is hidden/
    // zero-sized, and the event doesn't always drive lv_menu's pop), so
    // clear history and re-load the root page directly.
    if (menu) {
        lv_obj_t *page = lv_menu_get_cur_main_page(menu);
        if (page && page != settings_main_page) {
            lv_menu_clear_history(menu);
            lv_menu_set_page(menu, settings_main_page);
            return;
        }
    }
    // menu_show will trigger AppManager::switchApp which calls ui_sys_exit
    menu_show();
}
// Only "Inter" carries the Latin-1 supplement (U+00A0..U+00FF), so it's the
// face to pick when rendering Portuguese, Spanish, French, or other accented
// Latin scripts. The label below advertises that so users know which to pick.
static const char *FONT_FACE_OPTIONS = "Montserrat\nUnscii 8\nUnscii 16\nCourier\nInter (Latin-1)";
static const char *FONT_SIZE_OPTIONS = "10\n12\n14\n16\n18\n20\n22\n24\n26\n28\n30\n32";

static uint8_t size_to_idx(uint8_t size)
{
    if (size < 10 || size > 32) return 2; // Default 14
    return (size - 10) / 2;
}

// Persist every font change right away so the selection survives unexpected
// exits (crash, power loss, app switch that bypasses ui_sys_exit). The full
// blob is small and NVS writes are cheap enough for interactive use.
static void commit_font_change()
{
    hw_set_user_setting(local_param);
}

static void editor_font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.editor_font_index = lv_dropdown_get_selected(obj);
    commit_font_change();
    lv_event_stop_processing(e);
}

static void editor_font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.editor_font_size = 10 + lv_dropdown_get_selected(obj) * 2;
    commit_font_change();
    lv_event_stop_processing(e);
}

static void journal_font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.journal_font_index = lv_dropdown_get_selected(obj);
    commit_font_change();
    lv_event_stop_processing(e);
}

static void journal_font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.journal_font_size = 10 + lv_dropdown_get_selected(obj) * 2;
    commit_font_change();
    lv_event_stop_processing(e);
}

static void md_font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.md_font_index = lv_dropdown_get_selected(obj);
    commit_font_change();
    lv_event_stop_processing(e);
}

static void md_font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.md_font_size = 10 + lv_dropdown_get_selected(obj) * 2;
    commit_font_change();
    lv_event_stop_processing(e);
}

static void header_font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.header_font_index = lv_dropdown_get_selected(obj);
    commit_font_change();
    lv_event_stop_processing(e);
}

static void header_font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.header_font_size = 10 + lv_dropdown_get_selected(obj) * 2;
    commit_font_change();
    lv_event_stop_processing(e);
}

static void home_font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.home_font_index = lv_dropdown_get_selected(obj);
    commit_font_change();
    lv_event_stop_processing(e);
}

static void home_font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.home_font_size = 10 + lv_dropdown_get_selected(obj) * 2;
    commit_font_change();
    lv_event_stop_processing(e);
}

static void weather_font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.weather_font_index = lv_dropdown_get_selected(obj);
    commit_font_change();
    lv_event_stop_processing(e);
}

static void weather_font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.weather_font_size = 10 + lv_dropdown_get_selected(obj) * 2;
    commit_font_change();
    lv_event_stop_processing(e);
}

static void telegram_font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.telegram_font_index = lv_dropdown_get_selected(obj);
    commit_font_change();
}

static void telegram_font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.telegram_font_size = 10 + lv_dropdown_get_selected(obj) * 2;
    commit_font_change();
}

static void system_font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.system_font_index = lv_dropdown_get_selected(obj);
    commit_font_change();
    lv_event_stop_processing(e);
}

static void system_font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.system_font_size = 10 + lv_dropdown_get_selected(obj) * 2;
    commit_font_change();
    lv_event_stop_processing(e);
}

static void build_subpage_fonts(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    auto add_font_section = [&](const char *title, uint8_t face_idx, uint8_t size,
                                lv_event_cb_t face_cb, lv_event_cb_t size_cb) {
        lv_obj_t *row = lv_obj_create(sub_page);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, title);
        lv_obj_set_style_text_color(name, UI_COLOR_MUTED, 0);
        lv_obj_set_width(name, LV_PCT(26));

        lv_obj_t *dd_face = lv_dropdown_create(row);
        lv_dropdown_set_options(dd_face, FONT_FACE_OPTIONS);
        lv_dropdown_set_selected(dd_face, face_idx);
        lv_obj_set_width(dd_face, 0);
        lv_obj_set_flex_grow(dd_face, 2);
        if (face_cb) {
            lv_obj_add_event_cb(dd_face, face_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
        register_subpage_group_obj(sub_page, dd_face);

        lv_obj_t *dd_size = lv_dropdown_create(row);
        lv_dropdown_set_options(dd_size, FONT_SIZE_OPTIONS);
        lv_dropdown_set_selected(dd_size, size_to_idx(size));
        lv_obj_set_width(dd_size, 0);
        lv_obj_set_flex_grow(dd_size, 1);
        if (size_cb) {
            lv_obj_add_event_cb(dd_size, size_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
        register_subpage_group_obj(sub_page, dd_size);
    };

    add_font_section("System",   local_param.system_font_index,  local_param.system_font_size,
                     system_font_face_cb,  system_font_size_cb);
    add_font_section("Home",     local_param.home_font_index,    local_param.home_font_size,
                     home_font_face_cb,    home_font_size_cb);
    add_font_section("Notes",    local_param.editor_font_index,  local_param.editor_font_size,
                     editor_font_face_cb,  editor_font_size_cb);
    add_font_section("Journal",  local_param.journal_font_index, local_param.journal_font_size,
                     journal_font_face_cb, journal_font_size_cb);
    add_font_section("News",     local_param.md_font_index,      local_param.md_font_size,
                     md_font_face_cb,      md_font_size_cb);
    add_font_section("Weather",  local_param.weather_font_index, local_param.weather_font_size,
                     weather_font_face_cb, weather_font_size_cb);
    add_font_section("Telegram", local_param.telegram_font_index, local_param.telegram_font_size,
                     telegram_font_face_cb, telegram_font_size_cb);
    add_font_section("Header",   local_param.header_font_index,  local_param.header_font_size,
                     header_font_face_cb,  header_font_size_cb);
}

static lv_obj_t *create_subpage_fonts(lv_obj_t *menu, lv_obj_t *parent)
{
    lv_obj_t *cont = lv_menu_cont_create(parent);
    style_menu_item_icon(cont, LV_SYMBOL_EDIT, "Fonts");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_fonts);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static void storage_prefer_sd_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.storage_prefer_sd = en;
    hw_set_storage_prefer_sd(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " SD " : " Int ");
}

static void msc_target_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.msc_prefer_sd = en;
    hw_set_msc_prefer_sd(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " SD " : " Int ");
}

static void storage_prune_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.prune_internal = en;
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static struct {
    lv_obj_t *overlay;
    lv_obj_t *bar;
    lv_obj_t *label;
} storage_loader;

static void storage_progress_cb(int cur, int total, const char *name)
{
    if (!storage_loader.overlay) return;

    if (total > 0) {
        lv_bar_set_value(storage_loader.bar, cur * 100 / total, LV_ANIM_OFF);
        lv_label_set_text_fmt(storage_loader.label, "Processing (%d/%d):\n%s", cur, total, name);
    } else {
        lv_label_set_text(storage_loader.label, name ? name : "Working...");
    }
    lv_refr_now(NULL);
}

static void show_storage_loader(const char *title)
{
    storage_loader.overlay = ui_popup_create(title);

    storage_loader.label = lv_label_create(storage_loader.overlay);
    lv_label_set_text(storage_loader.label, "Preparing...");
    lv_obj_set_style_text_color(storage_loader.label, UI_COLOR_FG, 0);
    lv_obj_set_style_text_align(storage_loader.label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(storage_loader.label, lv_pct(80));

    storage_loader.bar = lv_bar_create(storage_loader.overlay);
    lv_obj_set_size(storage_loader.bar, lv_pct(70), 12);
    lv_bar_set_range(storage_loader.bar, 0, 100);
    lv_bar_set_value(storage_loader.bar, 0, LV_ANIM_OFF);

    lv_refr_now(NULL);
}

static void hide_storage_loader()
{
    if (storage_loader.overlay) {
        ui_popup_destroy(storage_loader.overlay);
        storage_loader.overlay = NULL;
    }
}

static void storage_prune_now_cb(lv_event_t *e) {
    show_storage_loader("Pruning Internal");
    hw_prune_internal_storage(storage_progress_cb);
    hide_storage_loader();
    ui_msg_pop_up("Storage", "Internal storage pruned to\nkeep only the 50 newest files.");
}

static void storage_copy_to_sd_cb(lv_event_t *e) {
    show_storage_loader("Backup to SD");

    int copied = 0, failed = 0;
    std::string err;
    bool ok = hw_copy_internal_to_sd(&copied, &failed, &err, storage_progress_cb);

    hide_storage_loader();

    char msg[128];
    if (ok) {
        snprintf(msg, sizeof(msg), "Copied %d file(s) to SD.", copied);
    } else if (!err.empty()) {
        snprintf(msg, sizeof(msg), "Copy failed: %s\nCopied: %d, failed: %d.",
                 err.c_str(), copied, failed);
    } else {
        snprintf(msg, sizeof(msg), "Copy failed.\nCopied: %d, failed: %d.",
                 copied, failed);
    }
    ui_msg_pop_up("Storage", msg);
}

static void toggle_child_focus_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * parent = lv_obj_get_parent(obj);
    if(code == LV_EVENT_FOCUSED) {
        lv_obj_add_state(parent, LV_STATE_FOCUSED);
        lv_obj_add_state(parent, LV_STATE_FOCUS_KEY);
    } else if(code == LV_EVENT_DEFOCUSED) {
        lv_obj_remove_state(parent, LV_STATE_FOCUSED);
        lv_obj_remove_state(parent, LV_STATE_FOCUS_KEY);
    }
}

static lv_obj_t *create_toggle_btn_row(lv_obj_t *parent, const char *txt, bool initial_state, lv_event_cb_t cb)
{
    lv_obj_t *obj = create_text(parent, NULL, txt, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *btn = lv_btn_create(obj);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE); 
    if (initial_state) lv_obj_add_state(btn, LV_STATE_CHECKED);
    
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_STATE_CHECKED);
    
    // Fix width to align "On/Off" texts to the right, like the standard labels
    lv_obj_set_width(btn, 60);
    lv_obj_set_flex_grow(btn, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, initial_state ? " On " : " Off ");
    lv_obj_center(label);
    
    lv_obj_set_user_data(btn, label);
    
    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_VALUE_CHANGED, NULL);

    return btn;
}

static void build_subpage_storage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    // USB MSC Toggle
    lv_obj_t *msc_row = create_text(sub_page, NULL, "USB MSC Target", LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *msc_btn = lv_btn_create(msc_row);
    lv_obj_add_flag(msc_btn, LV_OBJ_FLAG_CHECKABLE);
    bool msc_sd = local_param.msc_prefer_sd;
    if (msc_sd) lv_obj_add_state(msc_btn, LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(msc_btn, 0, 0);
    lv_obj_set_style_outline_width(msc_btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(msc_btn, 0, 0);
    lv_obj_set_style_border_width(msc_btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(msc_btn, UI_COLOR_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_width(msc_btn, 60);
    lv_obj_t *msc_btn_label = lv_label_create(msc_btn);
    lv_label_set_text(msc_btn_label, msc_sd ? " SD " : " Int ");
    lv_obj_center(msc_btn_label);
    lv_obj_set_user_data(msc_btn, msc_btn_label);
    lv_obj_add_event_cb(msc_btn, msc_target_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(msc_btn, toggle_child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(msc_btn, toggle_child_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    register_subpage_group_obj(sub_page, msc_btn);

    // Prune Internal Toggle
    lv_obj_t *btn = create_toggle_btn_row(sub_page, "Limit 50 files", local_param.prune_internal, storage_prune_cb);
    register_subpage_group_obj(sub_page, btn);

    // Action: copy all internal files to SD.
    lv_obj_t *copy_row = create_text(sub_page, NULL, "Copy Internal -> SD", LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *copy_btn = lv_btn_create(copy_row);
    lv_obj_set_width(copy_btn, 60);
    lv_obj_set_style_outline_width(copy_btn, 0, 0);
    lv_obj_set_style_outline_width(copy_btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(copy_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(copy_btn, 0, 0);
    lv_obj_set_style_border_width(copy_btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(copy_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_width(copy_btn, 0, 0);
    lv_obj_set_style_shadow_width(copy_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_width(copy_btn, 0, LV_STATE_FOCUS_KEY);

    lv_obj_t *copy_label = lv_label_create(copy_btn);
    lv_label_set_text(copy_label, LV_SYMBOL_COPY);
    lv_obj_center(copy_label);
    lv_obj_add_event_cb(copy_btn, storage_copy_to_sd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(copy_btn, toggle_child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(copy_btn, toggle_child_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    register_subpage_group_obj(sub_page, copy_btn);

    // Action: Manual prune
    lv_obj_t *prune_row = create_text(sub_page, NULL, "Prune Now (Keep 50)", LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *prune_btn = lv_btn_create(prune_row);
    lv_obj_set_width(prune_btn, 60);
    lv_obj_set_style_outline_width(prune_btn, 0, 0);
    lv_obj_set_style_outline_width(prune_btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(prune_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(prune_btn, 0, 0);
    lv_obj_set_style_border_width(prune_btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(prune_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_width(prune_btn, 0, 0);
    lv_obj_set_style_shadow_width(prune_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_width(prune_btn, 0, LV_STATE_FOCUS_KEY);

    lv_obj_t *prune_label = lv_label_create(prune_btn);
    lv_label_set_text(prune_label, LV_SYMBOL_TRASH);
    lv_obj_center(prune_label);
    lv_obj_add_event_cb(prune_btn, storage_prune_now_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(prune_btn, toggle_child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(prune_btn, toggle_child_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    register_subpage_group_obj(sub_page, prune_btn);
}

static lv_obj_t *create_subpage_storage(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_SD_CARD, "Storage");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_storage);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

/* ===== Notes Security (encryption-at-rest for *.txt notes) =====
 *
 * The flows are async because `ui_passphrase_prompt` is modal. State for a
 * multi-step flow (e.g. change passphrase) is kept in a tiny context struct
 * heap-allocated per invocation and threaded through the callbacks. */

namespace notes_sec {

/* Tracked so we can rebuild with the fresh enabled/unlocked state after a
 * passphrase operation — `settings_page_changed_cb` clears user_data after
 * the first build, so without this a re-entry would still show the old
 * "Status: OFF". */
static lv_obj_t *g_sub_page = nullptr;

struct ChangeCtx { std::string old_pw; };

static void progress_cb_relay(int cur, int total, const char *name)
{
    storage_progress_cb(cur, total, name);
}

static void show_loader(const char *title)
{
    show_storage_loader(title);
}

static void done_popup(const char *title, const char *msg)
{
    hide_storage_loader();
    ui_msg_pop_up(title, msg);
}

static void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);

static void mark_for_rebuild()
{
    if (!g_sub_page) return;
    /* Drop stale subpage_items entries for this page before cleaning, so
     * activate_subpage_group won't re-add deleted widgets. */
    for (uint8_t i = 0; i < subpage_item_count; ) {
        if (subpage_items[i].page == g_sub_page) {
            subpage_items[i] = subpage_items[--subpage_item_count];
        } else {
            i++;
        }
    }
    lv_obj_clean(g_sub_page);
    lv_obj_set_user_data(g_sub_page, (void*)&build_subpage);
}

static void return_to_main()
{
    /* Pop back to the settings root so the sub-page is rebuilt on re-entry
     * with the updated state. `menu` is the file-scope static owned by this
     * translation unit; the namespace makes it ambiguous so we skip the
     * refocus when it's unreachable. */
    mark_for_rebuild();
    if (settings_main_page && ::menu) {
        lv_menu_set_page(::menu, settings_main_page);
    }
}

/* --- Set passphrase flow --- */
static void set_pw_cb(const char *pw, void *ud)
{
    (void)ud;
    if (!pw) return;
    if (!notes_crypto_set_passphrase(pw)) {
        ui_msg_pop_up("Notes Security", "Could not set passphrase.");
        return;
    }
    show_loader("Encrypting notes");
    notes_crypto_encrypt_existing(progress_cb_relay);
    done_popup("Notes Security", "Passphrase set. Notes are now encrypted.");
    return_to_main();
}

/* --- Change passphrase flow: ask old, then ask new (with confirm). --- */
static void change_new_cb(const char *pw, void *ud)
{
    ChangeCtx *ctx = (ChangeCtx *)ud;
    if (!pw) { delete ctx; return; }
    std::string new_pw = pw;
    show_loader("Re-encrypting notes");
    bool ok = notes_crypto_change_passphrase(ctx->old_pw.c_str(),
                                             new_pw.c_str(),
                                             progress_cb_relay);
    delete ctx;
    done_popup("Notes Security",
               ok ? "Passphrase changed." : "Passphrase change failed.");
    return_to_main();
}

static void change_old_cb(const char *pw, void *ud)
{
    (void)ud;
    if (!pw) return;
    /* Verify old passphrase up front so the user doesn't type a whole new
     * one first only to find out the old was wrong. */
    if (!notes_crypto_unlock(pw)) {
        ui_msg_pop_up("Notes Security", "Wrong current passphrase.");
        return;
    }
    ChangeCtx *ctx = new ChangeCtx();
    ctx->old_pw = pw;
    ui_passphrase_prompt("New passphrase",
                         "Enter a new passphrase for your notes.",
                         /*confirm=*/true, change_new_cb, ctx);
}

/* --- Disable flow --- */
static void disable_pw_cb(const char *pw, void *ud)
{
    (void)ud;
    if (!pw) return;
    show_loader("Decrypting notes");
    bool ok = notes_crypto_disable(pw, progress_cb_relay);
    done_popup("Notes Security",
               ok ? "Encryption disabled. Notes are plaintext again."
                  : "Wrong passphrase.");
    return_to_main();
}

/* --- Sub-page wiring --- */

static void btn_set_cb(lv_event_t *e) {
    (void)e;
    ui_passphrase_prompt("Set passphrase",
                         "Create a passphrase. You'll need it to read your notes.",
                         /*confirm=*/true, set_pw_cb, NULL);
}

static void btn_change_cb(lv_event_t *e) {
    (void)e;
    ui_passphrase_prompt("Current passphrase",
                         "Enter your current passphrase.",
                         /*confirm=*/false, change_old_cb, NULL);
}

static void btn_disable_cb(lv_event_t *e) {
    (void)e;
    ui_passphrase_prompt("Disable encryption",
                         "Enter passphrase to decrypt all notes.",
                         /*confirm=*/false, disable_pw_cb, NULL);
}

static void btn_lock_cb(lv_event_t *e) {
    (void)e;
    notes_crypto_lock();
    ui_msg_pop_up("Notes Security", "Notes locked.");
    return_to_main();
}

static void btn_encrypt_sd_cb(lv_event_t *e) {
    (void)e;
    show_loader("Encrypting SD notes");
    int scanned = 0, enc_count = 0;
    bool ok = notes_crypto_encrypt_sd(&scanned, &enc_count, progress_cb_relay);
    char msg[96];
    if (!ok) {
        snprintf(msg, sizeof(msg), "SD card unavailable or session locked.");
    } else if (scanned == 0) {
        snprintf(msg, sizeof(msg), "No protected notes found on SD.");
    } else {
        snprintf(msg, sizeof(msg),
                 "Scanned %d file(s).\nEncrypted %d new file(s).",
                 scanned, enc_count);
    }
    done_popup("Notes Security", msg);
    return_to_main();
}

static void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);
    bool enabled  = notes_crypto_is_enabled();
    bool unlocked = notes_crypto_is_unlocked();

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    lv_obj_t *lbl = lv_label_create(status);
    const char *state_txt = !enabled ? "Status: OFF"
                          : unlocked ? "Status: Unlocked"
                                     : "Status: Locked";
    lv_label_set_text(lbl, state_txt);
    lv_obj_set_style_text_color(lbl, UI_COLOR_MUTED, 0);

    if (!enabled) {
        lv_obj_t *b = create_button(sub_page, LV_SYMBOL_KEYBOARD,
                                    "Set passphrase", btn_set_cb);
        register_subpage_group_obj(sub_page, b);
    } else {
        lv_obj_t *b1 = create_button(sub_page, LV_SYMBOL_REFRESH,
                                     "Change passphrase", btn_change_cb);
        register_subpage_group_obj(sub_page, b1);

        if (unlocked) {
            lv_obj_t *b2 = create_button(sub_page, LV_SYMBOL_CLOSE,
                                         "Lock now", btn_lock_cb);
            register_subpage_group_obj(sub_page, b2);

            lv_obj_t *b_sd = create_button(sub_page, LV_SYMBOL_SD_CARD,
                                           "Encrypt SD notes", btn_encrypt_sd_cb);
            register_subpage_group_obj(sub_page, b_sd);
        }

        lv_obj_t *b3 = create_button(sub_page, LV_SYMBOL_TRASH,
                                     "Disable encryption", btn_disable_cb);
        register_subpage_group_obj(sub_page, b3);
    }
}

} /* namespace notes_sec */

static void build_subpage_notes_security(lv_obj_t *menu, lv_obj_t *sub_page)
{
    notes_sec::build_subpage(menu, sub_page);
}

static lv_obj_t *create_subpage_notes_security(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_EYE_CLOSE, "Notes Security");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_notes_security);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    notes_sec::g_sub_page = sub_page;
    return cont;
}

// Connectivity subpage — "WiFi Networks" row is hidden when WiFi is off.
// `row` is the full menu_cont (icon + label + right-arrow btn); `btn` is the
// inner clickable that needs to join/leave the nav group. We hide the whole
// row so the label disappears along with the arrow.
static lv_obj_t *g_wifi_networks_row = nullptr;
static lv_obj_t *g_wifi_networks_btn = nullptr;
static lv_obj_t *g_connectivity_subpage = nullptr;

static void wifi_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.wifi_enable = en;
    hw_set_wifi_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
    if (g_wifi_networks_row) {
        if (en) {
            lv_obj_clear_flag(g_wifi_networks_row, LV_OBJ_FLAG_HIDDEN);
            if (g_wifi_networks_btn) lv_obj_clear_flag(g_wifi_networks_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_wifi_networks_row, LV_OBJ_FLAG_HIDDEN);
            if (g_wifi_networks_btn) lv_obj_add_flag(g_wifi_networks_btn, LV_OBJ_FLAG_HIDDEN);
        }
        // Rebuild the subpage nav group so the row is inserted at its
        // registered position (between the WiFi toggle and Bluetooth) rather
        // than tacked onto the end, and so it's actually focusable again.
        // activate_subpage_group defaults focus to the back button — move it
        // back to the WiFi toggle the user just interacted with.
        if (g_connectivity_subpage) {
            activate_subpage_group(g_connectivity_subpage);
            lv_group_focus_obj(obj);
        }
    }
}

static void bt_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.bt_enable = en;
    hw_set_bt_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static void radio_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.radio_enable = en;
    hw_set_radio_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static void nfc_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.nfc_enable = en;
    hw_set_nfc_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static void gps_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.gps_enable = en;
    hw_set_gps_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static void speaker_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.speaker_enable = en;
    hw_set_speaker_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static void haptic_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.haptic_enable = en;
    hw_set_haptic_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static void wifi_networks_click_cb(lv_event_t *e)
{
    (void)e;
    ui_wifi_networks_open();
}

static void build_subpage_connectivity(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    lv_obj_t *btn;

    btn = create_toggle_btn_row(sub_page, "WiFi", hw_get_wifi_enable(), wifi_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_button(sub_page, LV_SYMBOL_WIFI, "WiFi Networks", wifi_networks_click_cb);
    g_wifi_networks_btn = btn;
    g_wifi_networks_row = lv_obj_get_parent(btn);  // menu_cont holding icon+label+btn
    g_connectivity_subpage = sub_page;
    if (!hw_get_wifi_enable()) {
        lv_obj_add_flag(g_wifi_networks_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "Bluetooth", hw_get_bt_enable(), bt_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "Radio", hw_get_radio_enable(), radio_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "NFC", hw_get_nfc_enable(), nfc_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "GPS", hw_get_gps_enable(), gps_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "Speaker", hw_get_speaker_enable(), speaker_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "Haptic", hw_get_haptic_enable(), haptic_enable_cb);
    register_subpage_group_obj(sub_page, btn);
}

/* ===== Weather: choose the city used by the Weather app ===== */
namespace weather_cfg {

static lv_obj_t *g_sub_page = nullptr;
static lv_obj_t *g_city_label = nullptr;
static lv_obj_t *g_status_label = nullptr;
static std::vector<weather_city_match> g_search_results;

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

static void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
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

/* ===== Telegram: bridge URL + bearer token used by the Telegram app =====
 * The actual storage lives in ui_telegram.cpp (NVS + optional AES wrap via
 * notes crypto). This subpage is just the UI over `apps::tg_cfg_*`. */
namespace telegram_cfg {

static lv_obj_t *g_sub_page       = nullptr;
static lv_obj_t *g_url_label      = nullptr;
static lv_obj_t *g_tok_label      = nullptr;
static lv_obj_t *g_note_label     = nullptr;
static lv_obj_t *g_fav_sub_page   = nullptr;

static void build_favorites_subpage(lv_obj_t *menu, lv_obj_t *sub_page);

static void refresh_labels()
{
    if (g_url_label) {
        std::string url = apps::tg_cfg_get_url();
        lv_label_set_text_fmt(g_url_label, "URL: %s",
                              url.empty() ? "(not set)" : url.c_str());
    }
    if (g_tok_label) {
        std::string tok = apps::tg_cfg_get_token_display();
        const char *suffix = apps::tg_cfg_token_is_encrypted()
                             ? "  [encrypted]" : "  [plaintext]";
        lv_label_set_text_fmt(g_tok_label, "Token: %s%s",
                              tok.c_str(), suffix);
    }
#ifdef ARDUINO
    if (g_note_label) {
        const char *note;
        if (!notes_crypto_is_enabled()) {
            note = "Enable Notes encryption (Settings » Notes Security) "
                   "before saving the bearer — the token is persisted "
                   "only AES-256 wrapped, never plaintext.";
        } else if (!notes_crypto_is_unlocked()) {
            note = "Notes session locked — unlock (open Notes) to "
                   "save or read the token.";
        } else {
            note = "Token is AES-256 wrapped with your notes passphrase.";
        }
        lv_label_set_text(g_note_label, note);
    }
#endif
}

static void set_url_cb(const char *text, void *)
{
    if (!text) return;
    apps::tg_cfg_set_url(text);
    refresh_labels();
}

static void set_token_cb(const char *text, void *)
{
    if (!text) return;
    std::string err;
    if (!apps::tg_cfg_set_token(text, &err)) {
        ui_msg_pop_up("Token rejected",
                      err.empty() ? "Save failed." : err.c_str());
    }
    refresh_labels();
}

static void btn_set_url_cb(lv_event_t *)
{
    std::string current = apps::tg_cfg_get_url();
    ui_text_prompt("Bridge URL", "https://tg.example.com",
                   current.c_str(), set_url_cb, nullptr);
}

static void btn_open_favorites_cb(lv_event_t *)
{
    if (!menu || !g_fav_sub_page) return;
    lv_menu_set_page(menu, g_fav_sub_page);
}

static void btn_set_token_cb(lv_event_t *)
{
    // Intentionally don't pre-fill the token so we never read plaintext out
    // of NVS just to build a prompt — ui_text_prompt would echo it on-screen.
    ui_text_prompt("Bearer token", "from config.yaml on the bridge",
                   "", set_token_cb, nullptr);
}

// Notification toggles mirror the haptic_enable pattern above: the toggle
// btn holds the " On "/" Off " label as its user_data so the callback can
// flip the text in place when the user changes it.
static void notif_vibrate_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    apps::tg_cfg_set_notif_vibrate(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static void notif_banner_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    apps::tg_cfg_set_notif_banner(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(status, 2, 0);

    g_url_label = lv_label_create(status);
    lv_label_set_long_mode(g_url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_url_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_url_label, UI_COLOR_MUTED, 0);

    g_tok_label = lv_label_create(status);
    lv_label_set_long_mode(g_tok_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_tok_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_tok_label, UI_COLOR_MUTED, 0);

    lv_obj_t *b1 = create_button(sub_page, LV_SYMBOL_WIFI,
                                 "Set URL", btn_set_url_cb);
    register_subpage_group_obj(sub_page, b1);

    lv_obj_t *b2 = create_button(sub_page, LV_SYMBOL_KEYBOARD,
                                 "Set token", btn_set_token_cb);
    register_subpage_group_obj(sub_page, b2);

    // Favorites: clicking opens a nested subpage that fetches the full chat
    // list from the bridge and lets the user star/unstar each chat. The
    // Telegram app's listing renders only the starred ones. Match the
    // Set URL / Set token rows exactly — create_button (real cb) gives us the
    // same chevron bubble, and the cb triggers navigation via
    // lv_menu_set_page instead of relying on lv_menu_set_load_page_event
    // (which doesn't play well with a child btn inside the menu_cont).
    g_fav_sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(g_fav_sub_page, (void*)build_favorites_subpage);
    lv_obj_t *b3 = create_button(sub_page, LV_SYMBOL_EYE_OPEN,
                                 "Favorites", btn_open_favorites_cb);
    register_subpage_group_obj(sub_page, b3);

    // Per-channel notification toggles. The background poll (running even
    // when the Telegram app is closed) dispatches to these when it sees the
    // total unread count go up. Haptic respects the global Haptic toggle in
    // Connectivity — if that's off, enabling "Vibrate" here still won't
    // buzz.
    lv_obj_t *b4 = create_toggle_btn_row(sub_page, "Vibrate on new msg",
                                         apps::tg_cfg_get_notif_vibrate(),
                                         notif_vibrate_cb);
    register_subpage_group_obj(sub_page, b4);

    lv_obj_t *b5 = create_toggle_btn_row(sub_page, "Banner on new msg",
                                         apps::tg_cfg_get_notif_banner(),
                                         notif_banner_cb);
    register_subpage_group_obj(sub_page, b5);

#ifdef ARDUINO
    lv_obj_t *note_row = lv_menu_cont_create(sub_page);
    g_note_label = lv_label_create(note_row);
    lv_label_set_long_mode(g_note_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_note_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_note_label, UI_COLOR_MUTED, 0);
#endif

    refresh_labels();
}

// Per-button owned chat id — freed via LV_EVENT_DELETE since `long long`
// won't fit in a 32-bit void* slot on the ESP32-S3.
static void fav_id_delete_cb(lv_event_t *e)
{
    auto *id = (long long *)lv_event_get_user_data(e);
    delete id;
}

static void fav_toggle_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    auto *id = (long long *)lv_event_get_user_data(e);
    if (!id) return;
    bool checked = lv_obj_has_state(btn, LV_STATE_CHECKED);
    apps::tg_cfg_set_favorite(*id, checked);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(btn);
    if (label) lv_label_set_text(label, checked ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
}

static void build_favorites_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    lv_obj_t *status_row = lv_menu_cont_create(sub_page);
    lv_obj_t *status_lbl = lv_label_create(status_row);
    lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(status_lbl, UI_COLOR_MUTED, 0);
    lv_label_set_text(status_lbl, "Loading chats...");
    lv_refr_now(NULL);

    std::vector<std::pair<long long, std::string>> chats;
    std::string err;
    bool ok = apps::tg_cfg_fetch_all_chats(chats, &err);
    if (!ok) {
        lv_label_set_text_fmt(status_lbl, "Fetch failed: %s", err.c_str());
        return;
    }
    if (chats.empty()) {
        lv_label_set_text(status_lbl, "Bridge returned no chats.");
        return;
    }
    lv_label_set_text_fmt(status_lbl,
        "%u chat(s) — tap to toggle favorite.", (unsigned)chats.size());

    for (auto &c : chats) {
        long long id = c.first;
        const std::string &title = c.second;

        lv_obj_t *row = create_text(sub_page, NULL, title.c_str(),
                                    LV_MENU_ITEM_BUILDER_VARIANT_2);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(btn, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_FG, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_BG, LV_STATE_CHECKED);
        lv_obj_set_size(btn, 44, 28);
        lv_obj_set_flex_grow(btn, 0);

        bool starred = apps::tg_cfg_is_favorite(id);
        if (starred) lv_obj_add_state(btn, LV_STATE_CHECKED);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, starred ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
        lv_obj_center(label);
        lv_obj_set_user_data(btn, label);

        long long *id_heap = new long long(id);
        lv_obj_add_event_cb(btn, fav_toggle_cb, LV_EVENT_VALUE_CHANGED, id_heap);
        lv_obj_add_event_cb(btn, fav_id_delete_cb, LV_EVENT_DELETE, id_heap);
        register_subpage_group_obj(sub_page, btn);
    }
}

} // namespace telegram_cfg

/* ===== Notes Sync: GitHub repo + PAT used by the Notes Sync app =========
 * Storage lives in ui_notes_sync.cpp (NVS "notesync" namespace; token is
 * AES-wrapped when notes crypto is enabled, same pattern as Telegram).
 * This subpage is just the UI over `apps::nsync_cfg_*`. */
namespace notes_sync_cfg {

static lv_obj_t *g_sub_page    = nullptr;
static lv_obj_t *g_repo_label  = nullptr;
static lv_obj_t *g_branch_label = nullptr;
static lv_obj_t *g_news_label  = nullptr;
static lv_obj_t *g_tok_label   = nullptr;
static lv_obj_t *g_note_label  = nullptr;

static void refresh_labels()
{
    if (g_repo_label) {
        std::string v = apps::nsync_cfg_get_repo();
        lv_label_set_text_fmt(g_repo_label, "Repo: %s",
                              v.empty() ? "(not set)" : v.c_str());
    }
    if (g_branch_label) {
        std::string v = apps::nsync_cfg_get_branch();
        lv_label_set_text_fmt(g_branch_label, "Branch: %s", v.c_str());
    }
    if (g_news_label) {
        std::string v = apps::nsync_cfg_get_news_url();
        lv_label_set_text_fmt(g_news_label, "News: %s", v.c_str());
    }
    if (g_tok_label) {
        std::string tok = apps::nsync_cfg_get_token_display();
        const char *suffix = apps::nsync_cfg_token_is_encrypted()
                             ? "  [encrypted]" : "  [plaintext]";
        lv_label_set_text_fmt(g_tok_label, "Token: %s%s",
                              tok.c_str(), suffix);
    }
#ifdef ARDUINO
    if (g_note_label) {
        const char *note;
        if (!notes_crypto_is_enabled()) {
            note = "Enable Notes encryption (Settings » Notes Security) "
                   "before saving a PAT — the token is persisted only "
                   "AES-256 wrapped, never plaintext.";
        } else if (!notes_crypto_is_unlocked()) {
            note = "Notes session locked — unlock (open Notes) to "
                   "save or read the token.";
        } else {
            note = "Token is AES-256 wrapped with your notes passphrase. "
                   "Private repos work — use a fine-grained PAT with "
                   "Contents: read/write on just this repo.";
        }
        lv_label_set_text(g_note_label, note);
    }
#endif
}

static void set_repo_cb(const char *text, void *)
{
    if (!text) return;
    apps::nsync_cfg_set_repo(text);
    refresh_labels();
}

static void set_branch_cb(const char *text, void *)
{
    if (!text) return;
    apps::nsync_cfg_set_branch(text);
    refresh_labels();
}

static void set_news_cb(const char *text, void *)
{
    if (!text) return;
    apps::nsync_cfg_set_news_url(text);
    refresh_labels();
}

static void set_token_cb(const char *text, void *)
{
    if (!text) return;
    std::string err;
    if (!apps::nsync_cfg_set_token(text, &err)) {
        ui_msg_pop_up("Token rejected",
                      err.empty() ? "Save failed." : err.c_str());
    }
    refresh_labels();
}

static void btn_set_repo_cb(lv_event_t *)
{
    std::string current = apps::nsync_cfg_get_repo();
    ui_text_prompt("GitHub repo", "owner/repo",
                   current.c_str(), set_repo_cb, nullptr);
}

static void btn_set_branch_cb(lv_event_t *)
{
    std::string current = apps::nsync_cfg_get_branch();
    ui_text_prompt("Branch", "main",
                   current.c_str(), set_branch_cb, nullptr);
}

static void btn_set_news_cb(lv_event_t *)
{
    std::string current = apps::nsync_cfg_get_news_url();
    ui_text_prompt("News index URL", "https://...",
                   current.c_str(), set_news_cb, nullptr);
}

static void btn_set_token_cb(lv_event_t *)
{
    // No pre-fill so the PAT is never rendered back to the screen.
    ui_text_prompt("GitHub token",
                   "Personal access token (contents:write)",
                   "", set_token_cb, nullptr);
}

static void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(status, 2, 0);

    g_repo_label = lv_label_create(status);
    lv_label_set_long_mode(g_repo_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_repo_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_repo_label, UI_COLOR_MUTED, 0);

    g_branch_label = lv_label_create(status);
    lv_label_set_long_mode(g_branch_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_branch_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_branch_label, UI_COLOR_MUTED, 0);

    g_news_label = lv_label_create(status);
    lv_label_set_long_mode(g_news_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_news_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_news_label, UI_COLOR_MUTED, 0);

    g_tok_label = lv_label_create(status);
    lv_label_set_long_mode(g_tok_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_tok_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_tok_label, UI_COLOR_MUTED, 0);

    lv_obj_t *b1 = create_button(sub_page, LV_SYMBOL_DIRECTORY,
                                 "Set repo", btn_set_repo_cb);
    register_subpage_group_obj(sub_page, b1);

    lv_obj_t *b2 = create_button(sub_page, LV_SYMBOL_SHUFFLE,
                                 "Set branch", btn_set_branch_cb);
    register_subpage_group_obj(sub_page, b2);

    lv_obj_t *b3 = create_button(sub_page, LV_SYMBOL_KEYBOARD,
                                 "Set token", btn_set_token_cb);
    register_subpage_group_obj(sub_page, b3);

    lv_obj_t *b4 = create_button(sub_page, LV_SYMBOL_DOWNLOAD,
                                 "Set news URL", btn_set_news_cb);
    register_subpage_group_obj(sub_page, b4);

#ifdef ARDUINO
    lv_obj_t *note_row = lv_menu_cont_create(sub_page);
    g_note_label = lv_label_create(note_row);
    lv_label_set_long_mode(g_note_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_note_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_note_label, UI_COLOR_MUTED, 0);
#endif

    refresh_labels();
}

} // namespace notes_sync_cfg

static void build_subpage_weather(lv_obj_t *menu, lv_obj_t *sub_page)
{
    weather_cfg::build_subpage(menu, sub_page);
}

static void build_subpage_telegram(lv_obj_t *menu, lv_obj_t *sub_page)
{
    telegram_cfg::build_subpage(menu, sub_page);
}

static void build_subpage_notes_sync(lv_obj_t *menu, lv_obj_t *sub_page)
{
    notes_sync_cfg::build_subpage(menu, sub_page);
}

static lv_obj_t *create_subpage_weather(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_GPS, "Weather");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_weather);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    weather_cfg::g_sub_page = sub_page;
    return cont;
}

static lv_obj_t *create_subpage_telegram(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_ENVELOPE, "Telegram");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_telegram);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    telegram_cfg::g_sub_page = sub_page;
    return cont;
}

static lv_obj_t *create_subpage_notes_sync(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_REFRESH, "Notes Sync");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_notes_sync);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    notes_sync_cfg::g_sub_page = sub_page;
    return cont;
}

static lv_obj_t *create_subpage_connectivity(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_WIFI, "Connectivity");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_subpage_connectivity);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

void ui_sys_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    menu_g = lv_group_get_default();

    enable_keyboard();

    hw_get_user_setting(local_param);

    menu = lv_menu_create(parent);
#if LVGL_VERSION_MAJOR == 9
    lv_menu_set_mode_root_back_button(menu, LV_MENU_ROOT_BACK_BUTTON_DISABLED);
#else
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_DISABLED);
#endif
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(100));
    lv_obj_center(menu);

    // Suppress the menu's built-in header back button: LVGL re-un-hides it on
    // each subpage, so a plain HIDDEN flag on the header doesn't stick. Zero
    // its size and styling so the header's content_height stays 0 and LVGL's
    // own refr_main_header_mode hides the whole header automatically. It
    // remains programmatically clickable for the status bar back to drive.
    lv_obj_t *bb = lv_menu_get_main_header_back_button(menu);
    if (bb) {
        lv_obj_set_size(bb, 0, 0);
        lv_obj_set_style_pad_all(bb, 0, 0);
        lv_obj_set_style_border_width(bb, 0, 0);
        lv_obj_set_style_outline_width(bb, 0, 0);
        lv_obj_set_style_shadow_width(bb, 0, 0);
        lv_obj_set_style_bg_opa(bb, LV_OPA_TRANSP, 0);
    }

    /*Create a main page*/
    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);

    lv_obj_t *cont;
    main_page_group_count = 0;
    subpage_item_count = 0;

    // Flatten the item list into a two-column grid: each lv_menu_cont gets
    // LV_PCT(48) width and the page wraps rows of two.
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(main_page, 6, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    auto add_grid_item = [&](lv_obj_t *c) {
        if (!c) return;
        lv_obj_set_width(c, LV_PCT(48));
        add_main_page_group_item(c);
    };

    // Back button lives on the status bar. Keep the pointer so
    // restore_main_page_group() can focus it when returning from a subpage.
    settings_exit_btn = ui_show_back_button(settings_exit_cb);

    cont = create_subpage_backlight(menu, main_page);    add_grid_item(cont);
    cont = create_subpage_fonts(menu, main_page);        add_grid_item(cont);
    cont = create_subpage_datetime(menu, main_page);     add_grid_item(cont);
    cont = create_subpage_otg(menu, main_page);          add_grid_item(cont);
    cont = create_subpage_connectivity(menu, main_page); add_grid_item(cont);
    cont = create_subpage_weather(menu, main_page);      add_grid_item(cont);
    cont = create_subpage_telegram(menu, main_page);     add_grid_item(cont);
    cont = create_subpage_notes_sync(menu, main_page);   add_grid_item(cont);
    cont = create_subpage_storage(menu, main_page);      add_grid_item(cont);
    cont = create_files_item(main_page);                 add_grid_item(cont);
    cont = create_subpage_notes_security(menu, main_page); add_grid_item(cont);
    cont = create_subpage_performance(menu, main_page);  add_grid_item(cont);
    cont = create_subpage_info(menu, main_page);         add_grid_item(cont);
    cont = create_device_probe(menu, main_page);         add_grid_item(cont);
    cont = create_power_off_item(main_page);             add_grid_item(cont);

    settings_main_page = main_page;
    lv_menu_set_page(menu, main_page);
    lv_obj_add_event_cb(menu, settings_page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Without this the encoder has no focus anchor on the very first entry
    // (the menu tiles that used to hold focus were just cleaned up by
    // switchApp), so rotation does nothing until something is touched.
    if (main_page_group_count > 0) {
        lv_group_focus_obj(main_page_group_items[0]);
    } else if (settings_exit_btn) {
        lv_group_focus_obj(settings_exit_btn);
    }

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        settings_exit_cb(e);
    }, NULL);
#endif
}


void ui_sys_exit(lv_obj_t *parent)
{
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    ui_hide_back_button();
    // Intentionally do NOT disable_keyboard() here: the next app (e.g. Files
    // browser) immediately calls enable_keyboard(), which on T-LoRa-Pager
    // cycles the TCA8418 (kb.end()/kb.begin() → detach/reattach ISR + ledc).
    // That cycle races with the core-0 keyboard_task polling the same chip
    // and hangs the device. Keyboard is a system-level resource; leave it on
    // across app switches. Sleep path (ui_pause_timers) still disables it.
    if (menu) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
    }
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    hw_set_user_setting(local_param);
    // Rebuild the theme so any system-font change takes effect on the next
    // screen that gets constructed. Objects already created keep their old
    // font, but the next UI (menu, app, dialog) picks up the new one.
    theme_init();
    settings_main_page = NULL;
    settings_exit_btn = NULL;
    // Drop any in-flight NTP poll timer so it doesn't outlive the page it
    // was driving and touch a deleted status label.
    sync_stop_timer();
    dt_setup.sync_status = nullptr;
    g_wifi_networks_btn = nullptr;
    g_wifi_networks_row = nullptr;
    g_connectivity_subpage = nullptr;
    weather_cfg::g_sub_page = nullptr;
    weather_cfg::g_city_label = nullptr;
    telegram_cfg::g_sub_page = nullptr;
    telegram_cfg::g_url_label = nullptr;
    telegram_cfg::g_tok_label = nullptr;
    telegram_cfg::g_note_label = nullptr;
    notes_sync_cfg::g_sub_page = nullptr;
    notes_sync_cfg::g_repo_label = nullptr;
    notes_sync_cfg::g_branch_label = nullptr;
    notes_sync_cfg::g_news_label = nullptr;
    notes_sync_cfg::g_tok_label = nullptr;
    notes_sync_cfg::g_note_label = nullptr;
    main_page_group_count = 0;
    subpage_item_count = 0;
    memset(main_page_group_items, 0, sizeof(main_page_group_items));
    memset(subpage_items, 0, sizeof(subpage_items));
}

#include "apps/app_registry.h"

namespace {
class SysApp : public core::App {
public:
    SysApp() : core::App("Settings") {}
    void onStart(lv_obj_t *parent) override { ui_sys_enter(parent); }
    void onStop() override {
        ui_sys_exit(getRoot());
        core::App::onStop();
    }
};
} // namespace

namespace apps {
std::shared_ptr<core::App> make_sys_app() {
    return std::make_shared<SysApp>();
}
} // namespace apps
