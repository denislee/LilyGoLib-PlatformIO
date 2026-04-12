/**
 * @file      ui_sys.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"

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
static lv_group_t *menu_g;
static  user_setting_params_t local_param;
static uint32_t get_ip_id = 0;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *settings_main_page = NULL;
static lv_obj_t *settings_exit_btn = NULL;

void ui_sys_exit(lv_obj_t *parent);

#define MAX_MAIN_PAGE_ITEMS 8
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
    lv_obj_t *back_btn = lv_menu_get_main_header_back_button(menu);
    if (back_btn) {
        lv_group_add_obj(menu_g, back_btn);
    }
    for (uint8_t i = 0; i < subpage_item_count; i++) {
        if (subpage_items[i].page == page) {
            lv_group_add_obj(menu_g, subpage_items[i].obj);
        }
    }
    if (back_btn) {
        lv_group_focus_obj(back_btn);
    }
}

typedef struct {

    lv_obj_t *datetime_label;
    lv_obj_t *wifi_rssi_label;
    lv_obj_t *batt_voltage_label;

    lv_obj_t *wifi_ssid_label;
    lv_obj_t *ip_info_label;
    lv_obj_t *sd_size_label;
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

        float size = hw_get_sd_size();
        char buffer[64];
#if defined(HAS_SD_CARD_SOCKET)
        const char *unit = "GB";
#else
        const char *unit = "MB";
#endif
        if (size > 0) {
            snprintf(buffer, sizeof(buffer), "%.2f %s", size, unit);
        } else {
            snprintf(buffer, sizeof(buffer), "N/A");
        }
        if (sys_label.sd_size_label) lv_label_set_text(sys_label.sd_size_label, buffer);

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

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        if (timer) {
            lv_timer_del(timer);
            timer = NULL;
        }
        
        menu_show();

        lv_obj_clean(menu);
        lv_obj_del(menu);
        hw_set_user_setting(local_param);

        if (quit_btn) {
            lv_obj_del_async(quit_btn);
        }
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
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t val =  lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);
    lv_label_set_text_fmt(slider_label, "   %u%%  ", map_r(val, 0, 255, 0, 100));
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
        bool turnOn = lv_slider_get_value(obj) == 1;
        lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);
        printf("State: %s\n", turnOn ? "On" : "Off");
        if (hw_set_otg(turnOn) == false) {
            lv_slider_set_value(obj, 0, LV_ANIM_OFF);
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
        bool turnOn = lv_slider_get_value(obj) == 1;
        lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);
        local_param.charger_enable = turnOn;
        printf("State: %s\n", turnOn ? "On" : "Off");
        hw_set_charger(turnOn);
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
}

static void charge_limit_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(slider);
    int32_t val = lv_slider_get_value(slider);
    local_param.charge_limit_en = val;
    lv_label_set_text(slider_label, val ? " On " : " Off ");
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
        else if (key == 'e' || key == 'E' || key == LV_KEY_DOWN || key == LV_KEY_ENTER) {
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
} datetime_setup_t;

static void save_datetime_cb(lv_event_t *e)
{
    datetime_setup_t *setup = (datetime_setup_t *)lv_event_get_user_data(e);
    struct tm timeinfo = {0};

    timeinfo.tm_year = lv_spinbox_get_value(setup->year) - 1900;
    timeinfo.tm_mon = lv_spinbox_get_value(setup->mon) - 1;
    timeinfo.tm_mday = lv_spinbox_get_value(setup->day);
    timeinfo.tm_hour = lv_spinbox_get_value(setup->hour);
    timeinfo.tm_min = lv_spinbox_get_value(setup->min);
    timeinfo.tm_sec = 0;

    hw_set_date_time(timeinfo);
    lv_menu_set_page(menu, settings_main_page);
}

static lv_obj_t *create_subpage_datetime(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_BELL, "Date & Time");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(sub_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sub_page, 5, 0);
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    struct tm timeinfo;
    hw_get_date_time(timeinfo);

    static datetime_setup_t setup;

    /* --- Date row: Year / Month / Day --- */
    lv_obj_t *date_row = lv_obj_create(sub_page);
    lv_obj_set_size(date_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(date_row, 4, 0);
    lv_obj_set_style_pad_column(date_row, 6, 0);
    lv_obj_set_style_border_width(date_row, 0, 0);

    auto create_sb = [&](lv_obj_t *parent, const char *title, int min, int max, int val, int digits) {
        lv_obj_t *col = lv_obj_create(parent);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(col, 2, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);

        lv_obj_t *l = lv_label_create(col);
        lv_label_set_text(l, title);
        lv_obj_set_style_text_color(l, lv_palette_main(LV_PALETTE_GREY), 0);

        lv_obj_t *sb = lv_spinbox_create(col);
        lv_spinbox_set_range(sb, min, max);
        lv_spinbox_set_digit_format(sb, digits, 0);
        lv_spinbox_set_value(sb, val);
        lv_obj_set_width(sb, digits == 4 ? 70 : 50);
        lv_obj_add_event_cb(sb, spinbox_event_cb, LV_EVENT_ALL, NULL);
        register_subpage_group_obj(sub_page, sb);
        return sb;
    };

    setup.year  = create_sb(date_row, "Year",  2000, 2099, timeinfo.tm_year + 1900, 4);
    setup.mon   = create_sb(date_row, "Mon",   1, 12, timeinfo.tm_mon + 1, 2);
    setup.day   = create_sb(date_row, "Day",   1, 31, timeinfo.tm_mday, 2);

    /* --- Time row: Hour / Min --- */
    lv_obj_t *time_row = lv_obj_create(sub_page);
    lv_obj_set_size(time_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(time_row, 4, 0);
    lv_obj_set_style_pad_column(time_row, 6, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);

    setup.hour  = create_sb(time_row, "Hour",  0, 23, timeinfo.tm_hour, 2);

    lv_obj_t *colon = lv_label_create(time_row);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_bottom(colon, 0, 0);

    setup.min   = create_sb(time_row, "Min",   0, 59, timeinfo.tm_min, 2);

    /* --- Apply button --- */
    lv_obj_t *btn_row = lv_obj_create(sub_page);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);

    lv_obj_t *save_btn = lv_btn_create(btn_row);
    lv_obj_set_width(save_btn, LV_PCT(100));
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, LV_SYMBOL_OK " Apply");
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save_btn, save_datetime_cb, LV_EVENT_CLICKED, &setup);
    register_subpage_group_obj(sub_page, save_btn);

    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static lv_obj_t *create_subpage_backlight(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_IMAGE, "Display & Backlight");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
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

    if (hw_has_keyboard()) {
        add_slider("Keyboard", 0, 255,
                   local_param.keyboard_bl_level, keyboard_brightness_cb, "%d%%");
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

    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static lv_obj_t *create_subpage_otg(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_CHARGE, "Charger");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    bool enableOtg = hw_get_otg_enable();
    uint8_t total_charge_level = hw_get_charge_level_nums();
    uint8_t curr_charge_level = hw_get_charger_current_level();

    if (hw_has_otg_function()) {
        lv_obj_t *slider = create_slider(sub_page, NULL, "OTG Output",
                                         0, 1, enableOtg ? 1 : 0,
                                         otg_output_cb, LV_EVENT_VALUE_CHANGED);
        lv_obj_t *parent = lv_obj_get_parent(slider);
        lv_obj_t *slider_label = lv_label_create(parent);
        lv_label_set_text(slider_label, enableOtg ? " On " : " Off ");
        lv_obj_set_user_data(slider, slider_label);
        register_subpage_group_obj(sub_page, slider);
    }

    lv_obj_t *slider = create_slider(sub_page, NULL, "Charging",
                                     0, 1, local_param.charger_enable ? 1 : 0,
                                     charger_enable_cb, LV_EVENT_VALUE_CHANGED);
    lv_obj_t *parent = lv_obj_get_parent(slider);
    lv_obj_t *slider_label = lv_label_create(parent);
    lv_label_set_text(slider_label, local_param.charger_enable ? " On " : " Off ");
    lv_obj_set_user_data(slider, slider_label);
    register_subpage_group_obj(sub_page, slider);

    slider = create_slider(sub_page, NULL, "Current",
                                     1, total_charge_level, curr_charge_level,
                                     charger_current_cb, LV_EVENT_VALUE_CHANGED);
    parent = lv_obj_get_parent(slider);
    slider_label = lv_label_create(parent);
    lv_label_set_text_fmt(slider_label, "%umA", local_param.charger_current);
    lv_obj_set_user_data(slider, slider_label);
    register_subpage_group_obj(sub_page, slider);

    slider = create_slider(sub_page, NULL, "Limit 80%",
                                     0, 1, local_param.charge_limit_en ? 1 : 0,
                                     charge_limit_cb, LV_EVENT_VALUE_CHANGED);
    parent = lv_obj_get_parent(slider);
    slider_label = lv_label_create(parent);
    lv_label_set_text(slider_label, local_param.charge_limit_en ? " On " : " Off ");
    lv_obj_set_user_data(slider, slider_label);
    register_subpage_group_obj(sub_page, slider);

    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static lv_obj_t *create_subpage_info(lv_obj_t *menu, lv_obj_t *main_page)
{
    sys_label.info_loaded = false;
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_LIST, "System Info");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
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
    const char *storage_name = "SD Card";
#else
    const char *storage_name = "Storage";
#endif
    sys_label.sd_size_label = add_info_row(storage_name, "Loading...");

    snprintf(buffer, sizeof(buffer), "%d.%d.%d", lv_version_major(), lv_version_minor(), lv_version_patch());
    add_info_row("LVGL", buffer);

    string ver;
    hw_get_arduino_version(ver);
    add_info_row("Core", ver.c_str());

    add_info_row("Built", __DATE__);
    add_info_row("Hash", hw_get_firmware_hash_string());
    add_info_row("Chip", hw_get_chip_id_string());

    lv_menu_set_load_page_event(menu, cont, sub_page);

    timer = lv_timer_create(sys_timer_event_cb, 1000, NULL);

    return cont;
}

static lv_obj_t *create_device_probe(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_USB, "Devices");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
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

    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}


static void settings_page_changed_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    // Ignore events bubbling from children (like dropdowns)
    if (lv_event_get_target(e) != obj) return;

    lv_obj_t *page = lv_menu_get_cur_main_page(obj);
    if (page != settings_main_page) {
        activate_subpage_group(page);
    } else {
        restore_main_page_group();
    }
}

static void settings_exit_cb(lv_event_t *e)
{
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    
    ui_sys_exit(NULL);
    
    if (menu) {
        lv_obj_remove_event_cb(menu, settings_page_changed_cb);
    }

    menu_show();
    lv_refr_now(NULL); // Force refresh to show menu first

    lv_obj_clean(menu);
    lv_obj_del(menu);
    menu = NULL;
    settings_main_page = NULL;
    settings_exit_btn = NULL;
    hw_set_user_setting(local_param);

    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
}

static void editor_font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    local_param.editor_font_index = lv_dropdown_get_selected(obj);
    lv_event_stop_processing(e);
}

static void editor_font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint16_t index = lv_dropdown_get_selected(obj);
    local_param.editor_font_size = 10 + index * 2;
    lv_event_stop_processing(e);
}

static lv_obj_t *create_subpage_editor_settings(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_EDIT, "Editor Settings");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    const char *font_options = "Montserrat\nUnscii 8\nUnscii 16\nCourier";
    lv_obj_t *dd_face = create_dropdown(sub_page, NULL, "Font Face", font_options, local_param.editor_font_index, editor_font_face_cb);
    register_subpage_group_obj(sub_page, dd_face);

    const char *size_options = "10\n12\n14\n16\n18\n20\n22\n24\n26\n28\n30\n32";
    uint8_t size_idx = 2; // Default 14
    if (local_param.editor_font_size >= 10 && local_param.editor_font_size <= 32) {
        size_idx = (local_param.editor_font_size - 10) / 2;
    }
    lv_obj_t *dd_size = create_dropdown(sub_page, NULL, "Font Size", size_options, size_idx, editor_font_size_cb);
    register_subpage_group_obj(sub_page, dd_size);

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

    /*Create a main page*/
    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);

    lv_obj_t *cont;
    main_page_group_count = 0;
    subpage_item_count = 0;

    // //! EXIT BUTTON (FIRST)
    cont = lv_menu_cont_create(main_page);
    settings_exit_btn = lv_btn_create(cont);
    lv_obj_t *exit_label = lv_label_create(settings_exit_btn);
    lv_label_set_text(exit_label, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(settings_exit_btn, settings_exit_cb, LV_EVENT_CLICKED, NULL);
    add_main_page_group_item(settings_exit_btn);

    // //! BACKLIGHT SETTING
    cont = create_subpage_backlight(menu, main_page);
    add_main_page_group_item(cont);

    // //! DATE & TIME SETTING
    cont = create_subpage_datetime(menu, main_page);
    add_main_page_group_item(cont);

    // //! EDITOR SETTINGS
    cont = create_subpage_editor_settings(menu, main_page);
    add_main_page_group_item(cont);

    // //! SYSTEM INFO
    cont = create_subpage_info(menu, main_page);
    add_main_page_group_item(cont);

    cont = create_device_probe(menu, main_page);
    add_main_page_group_item(cont);

    cont = create_subpage_otg(menu, main_page);
    add_main_page_group_item(cont);

    settings_main_page = main_page;
    lv_menu_set_page(menu, main_page);
    lv_obj_add_event_cb(menu, settings_page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

#ifdef USING_TOUCHPAD
    quit_btn  = create_floating_button([](lv_event_t*e) {
        lv_obj_t *page = lv_menu_get_cur_main_page(menu);
        if (page == settings_main_page) {
             settings_exit_cb(NULL);
        } else {
             lv_menu_set_page(menu, settings_main_page);
        }
    }, NULL);
#endif
}


void ui_sys_exit(lv_obj_t *parent)
{
    disable_keyboard();
}

app_t ui_sys_main = {
    .setup_func_cb = ui_sys_enter,
    .exit_func_cb = ui_sys_exit,
    .user_data = nullptr,
};
