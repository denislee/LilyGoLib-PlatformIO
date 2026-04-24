/**
 * @file      settings_datetime.cpp
 * @brief     Settings » Date & Time subpage. Extracted from ui_settings.cpp;
 *            see settings_internal.h for the cross-TU contract.
 *
 * Three sections: manual spinbox entry, IANA timezone picker (via
 * timezone_fetch_list / timezone_fetch_offset in ui_time_sync.cpp), and
 * "Sync from Internet" which kicks SNTP and polls status.
 */
#include "../ui_define.h"
#include "../ui_list_picker.h"
#include "settings_internal.h"

#include <ctime>

namespace datetime_cfg {

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

static void sync_stop_timer()
{
    if (dt_setup.sync_timer) {
        lv_timer_del(dt_setup.sync_timer);
        dt_setup.sync_timer = nullptr;
    }
}

void reset_state()
{
    sync_stop_timer();
    dt_setup.sync_status = nullptr;
    dt_setup.tz_label    = nullptr;
    dt_setup.year = dt_setup.mon = dt_setup.day = nullptr;
    dt_setup.hour = dt_setup.min = nullptr;
    g_tz_candidates.clear();
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

    settings_return_to_main_page();
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

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
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

} // namespace datetime_cfg
