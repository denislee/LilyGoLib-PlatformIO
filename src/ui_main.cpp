/**
 * @file      ui.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-04
 *
 */
#include "ui_define.h"

lv_obj_t *main_screen;
static lv_obj_t *menu_panel;
static lv_obj_t *app_panel;
static lv_obj_t *home_list;
static lv_group_t *menu_g;
static lv_group_t *app_g;
static lv_timer_t *disp_timer;
static lv_timer_t *dev_timer;
static bool low_power_mode_flag = false;

static lv_obj_t *status_bar = NULL;
static lv_obj_t *stat_time_label = NULL;
static lv_obj_t *stat_batt_label = NULL;

void set_low_power_mode_flag(bool enable)
{
    low_power_mode_flag = enable;
}

void menu_show()
{
    set_default_group(menu_g);
    if (app_g) {
        lv_group_remove_all_objs(app_g);
    }
    lv_tileview_set_tile_by_index(main_screen, 0, 0, LV_ANIM_OFF);
    lv_timer_resume(disp_timer);
    lv_display_trigger_activity(NULL);
    hw_feedback();
    
    if (home_list && lv_obj_get_child_count(home_list) > 0) {
        lv_group_focus_obj(lv_obj_get_child(home_list, 0));
    }
}

void menu_hidden()
{
    lv_tileview_set_tile_by_index(main_screen, 0, 1, LV_ANIM_OFF);
    // Do NOT pause disp_timer, it's needed for the status bar
}

bool isinMenu()
{
    if (!main_screen) return true;
    return (lv_tileview_get_tile_act(main_screen) == menu_panel);
}

void set_default_group(lv_group_t *group)
{
    lv_group_set_default(group);
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        lv_indev_set_group(indev, group);
        indev = lv_indev_get_next(indev);
    }
}

static void btn_event_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    app_t *app = (app_t *)lv_event_get_user_data(e);
    if (c == LV_EVENT_CLICKED) {
        set_default_group(app_g);
        hw_feedback();
        if (app->setup_func_cb) {
            (*app->setup_func_cb)(app_panel);
        }
        if (isinMenu()) {
            menu_hidden();
        }
    }
}

static void shutdown_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        hw_feedback();
        lv_delay_ms(200); 
        hw_shutdown();
    }
}

static void style_list_btn_icon(lv_obj_t *btn)
{
    lv_obj_t *icon = lv_obj_get_child(btn, 0);
    if (icon) {
        lv_obj_set_style_min_width(icon, 20, 0);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void create_app(lv_obj_t *list, const char *name, const char *symbol, app_t *app_fun)
{
    lv_obj_t *btn = lv_list_add_btn(list, symbol, name);
    style_list_btn_icon(btn);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, app_fun);
    lv_group_add_obj(menu_g, btn);
}

static void ui_poll_timer_callback(lv_timer_t *t)
{
    if (stat_time_label && stat_batt_label) {
        // Update Time & Date
        struct tm timeinfo;
        hw_get_date_time(timeinfo);
        lv_label_set_text_fmt(stat_time_label, "%02d/%02d %02d:%02d", 
                            timeinfo.tm_mon + 1, timeinfo.tm_mday,
                            timeinfo.tm_hour, timeinfo.tm_min);

        // Update Battery
        monitor_params_t params;
        hw_get_monitor_params(params);
        
        const char *batt_sym = LV_SYMBOL_BATTERY_FULL;
        if (params.is_charging) {
            batt_sym = LV_SYMBOL_CHARGE;
        } else {
            if (params.battery_percent < 20) batt_sym = LV_SYMBOL_BATTERY_EMPTY;
            else if (params.battery_percent < 50) batt_sym = LV_SYMBOL_BATTERY_1;
            else if (params.battery_percent < 80) batt_sym = LV_SYMBOL_BATTERY_2;
        }
        
        lv_label_set_text_fmt(stat_batt_label, "%s %d%%", batt_sym, params.battery_percent);
    }

    user_setting_params_t settings;
    hw_get_user_setting(settings);

    if (low_power_mode_flag && settings.disp_timeout_second > 0) {
        uint32_t timeout_ms = (uint32_t)settings.disp_timeout_second * 1000UL;
        if (lv_display_get_inactive_time(NULL) > timeout_ms) {
            hw_low_power_loop();
        }
    }
}

static void hw_device_poll(lv_timer_t *t)
{
}

void setupGui()
{
    disable_keyboard();

    theme_init();

    menu_g = lv_group_create();
    lv_group_set_wrap(menu_g, false);
    app_g = lv_group_create();
    lv_group_set_wrap(app_g, false);
    set_default_group(menu_g);

    int32_t v_res = lv_display_get_vertical_resolution(NULL);
    if (v_res <= 0) v_res = 222; // Fallback

    // 1. Create Main Screen (TileView) FIRST
    main_screen = lv_tileview_create(lv_screen_active());
    lv_obj_set_size(main_screen, LV_PCT(100), v_res - 30);
    lv_obj_align(main_screen, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(main_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(main_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(main_screen, LV_OBJ_FLAG_SCROLL_ELASTIC);

    menu_panel = lv_tileview_add_tile(main_screen, 0, 0, LV_DIR_NONE);
    app_panel = lv_tileview_add_tile(main_screen, 0, 1, LV_DIR_NONE);

    // 2. Create Status Bar LAST so it stays on top of the main_screen
    status_bar = lv_obj_create(lv_screen_active());
    lv_obj_set_size(status_bar, LV_PCT(100), 30);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    stat_time_label = lv_label_create(status_bar);
    lv_obj_center(stat_time_label);
    lv_obj_set_style_text_color(stat_time_label, lv_color_white(), 0);

    stat_batt_label = lv_label_create(status_bar);
    lv_obj_align(stat_batt_label, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_text_color(stat_batt_label, lv_color_white(), 0);

    home_list = lv_list_create(menu_panel);
    lv_obj_set_size(home_list, LV_PCT(100), LV_PCT(100));
    lv_obj_center(home_list);
    lv_obj_set_style_bg_color(home_list, lv_color_black(), 0);
    lv_obj_set_style_border_width(home_list, 0, 0);

    extern app_t ui_sys_main;
    extern app_t ui_text_editor_main;
    extern app_t ui_file_browser_main;
    extern app_t ui_blog_main;

    create_app(home_list, "Editor", LV_SYMBOL_KEYBOARD, &ui_text_editor_main);
    create_app(home_list, "Blog", LV_SYMBOL_LIST, &ui_blog_main);
    create_app(home_list, "Settings", LV_SYMBOL_SETTINGS, &ui_sys_main);
    create_app(home_list, "Files", LV_SYMBOL_FILE, &ui_file_browser_main);

    lv_obj_t *shutdown_btn = lv_list_add_btn(home_list, LV_SYMBOL_POWER, "Shutdown");
    style_list_btn_icon(shutdown_btn);
    lv_obj_add_event_cb(shutdown_btn, shutdown_event_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(menu_g, shutdown_btn);

    disp_timer = lv_timer_create(ui_poll_timer_callback, 1000, NULL);
    dev_timer = lv_timer_create(hw_device_poll, 5000, NULL);

    // Initial update so it's not blank for the first second
    ui_poll_timer_callback(NULL);

    set_low_power_mode_flag(true);
    lv_display_trigger_activity(NULL);

    menu_show();

    // Boot directly to Editor
    set_default_group(app_g);
    if (ui_text_editor_main.setup_func_cb) {
        (*ui_text_editor_main.setup_func_cb)(app_panel);
    }
    menu_hidden();
}
