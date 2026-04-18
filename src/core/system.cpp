/**
 * @file      system.cpp
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#include "system.h"
#include "../ui_define.h"
#include <Arduino.h>

#include "../apps/menu_app.h"

// Definition of global UI objects
lv_obj_t *main_screen = nullptr;
lv_obj_t *menu_panel = nullptr;
lv_obj_t *app_panel = nullptr;
lv_group_t *menu_g = nullptr;
lv_group_t *app_g = nullptr;

namespace core {

System& System::getInstance() {
    static System instance;
    return instance;
}

void System::init() {
    setupGlobalUI();
    
    // Register the MainMenu app
    AppManager::getInstance().registerApp(std::make_shared<apps::MenuApp>());
    
    // Start with the menu
    AppManager::getInstance().switchApp("MainMenu", _menuPanel);
    showMenu();
}

void System::setupGlobalUI() {
    theme_init();

    _menuGroup = lv_group_create();
    lv_group_set_wrap(_menuGroup, false);
    _appGroup = lv_group_create();
    lv_group_set_wrap(_appGroup, false);
    
    // Set legacy globals
    menu_g = _menuGroup;
    app_g = _appGroup;

    // Set default group to menu group initially
    lv_group_set_default(_menuGroup);

    int32_t v_res = lv_display_get_vertical_resolution(NULL);
    if (v_res <= 0) v_res = 222;

    const lv_font_t *header_font = get_header_font();
    int32_t bar_h = lv_font_get_line_height(header_font) + 8;
    if (bar_h < 30) bar_h = 30;

    // Create Main Screen (TileView)
    _mainScreen = lv_tileview_create(lv_screen_active());
    main_screen = _mainScreen; // Set global pointer
    lv_obj_set_size(_mainScreen, LV_PCT(100), v_res - bar_h);
    lv_obj_align(_mainScreen, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_mainScreen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_mainScreen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(_mainScreen, LV_SCROLLBAR_MODE_OFF);

    _menuPanel = lv_tileview_add_tile(_mainScreen, 0, 0, LV_DIR_NONE);
    _appPanel = lv_tileview_add_tile(_mainScreen, 0, 1, LV_DIR_NONE);
    
    // Set legacy globals
    menu_panel = _menuPanel;
    app_panel = _appPanel;

    // Create Status Bar
    _statusBar = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_statusBar, LV_PCT(100), bar_h);
    lv_obj_align(_statusBar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_statusBar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_statusBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_statusBar, 0, 0);
    lv_obj_set_style_radius(_statusBar, 0, 0);
    lv_obj_set_style_pad_all(_statusBar, 2, 0);

    _statTimeLabel = lv_label_create(_statusBar);
    lv_obj_center(_statTimeLabel);
    lv_obj_set_style_text_color(_statTimeLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(_statTimeLabel, header_font, 0);

    _statBattLabel = lv_label_create(_statusBar);
    lv_obj_align(_statBattLabel, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_text_color(_statBattLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(_statBattLabel, header_font, 0);

    _statMemLabel = lv_label_create(_statusBar);
    lv_obj_align(_statMemLabel, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_text_color(_statMemLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(_statMemLabel, header_font, 0);
    lv_obj_add_flag(_statMemLabel, LV_OBJ_FLAG_HIDDEN);

    _statSDLabel = lv_label_create(_statusBar);
    lv_obj_align(_statSDLabel, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_text_color(_statSDLabel, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(_statSDLabel, header_font, 0);
    lv_label_set_text(_statSDLabel, LV_SYMBOL_SD_CARD);
    lv_obj_add_flag(_statSDLabel, LV_OBJ_FLAG_HIDDEN);

    _statUSBLabel = lv_label_create(_statusBar);
    lv_obj_align(_statUSBLabel, LV_ALIGN_LEFT_MID, 25, 0); // Next to SD label
    lv_obj_set_style_text_color(_statUSBLabel, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_text_font(_statUSBLabel, header_font, 0);
    lv_label_set_text(_statUSBLabel, LV_SYMBOL_USB);
    lv_obj_add_flag(_statUSBLabel, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create([](lv_timer_t *t) {
        System& self = System::getInstance();

        static const lv_font_t *applied_header_font = nullptr;
        const lv_font_t *cur_font = get_header_font();
        if (applied_header_font == nullptr) applied_header_font = cur_font;
        if (self._statusBar && cur_font != applied_header_font) {
            applied_header_font = cur_font;
            int32_t new_bar_h = lv_font_get_line_height(cur_font) + 8;
            if (new_bar_h < 30) new_bar_h = 30;
            lv_obj_set_style_text_font(self._statTimeLabel, cur_font, 0);
            lv_obj_set_style_text_font(self._statBattLabel, cur_font, 0);
            lv_obj_set_style_text_font(self._statMemLabel, cur_font, 0);
            lv_obj_set_style_text_font(self._statSDLabel, cur_font, 0);
            lv_obj_set_style_text_font(self._statUSBLabel, cur_font, 0);
            if (lv_obj_get_height(self._statusBar) != new_bar_h) {
                lv_obj_set_height(self._statusBar, new_bar_h);
                int32_t v_res = lv_display_get_vertical_resolution(NULL);
                if (v_res <= 0) v_res = 222;
                lv_obj_set_height(self._mainScreen, v_res - new_bar_h);
            }
        }

        // Status bar update logic
        struct tm timeinfo;
        hw_get_date_time(timeinfo);
        if (self._statTimeLabel) {
            lv_label_set_text_fmt(self._statTimeLabel, "%02d/%02d/%04d %02d:%02d", 
                                timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
                                timeinfo.tm_hour, timeinfo.tm_min);
        }

        monitor_params_t params;
        hw_get_monitor_params(params);
        if (self._statBattLabel) {
            const char *batt_sym = LV_SYMBOL_BATTERY_FULL;
            if (params.is_charging) batt_sym = LV_SYMBOL_CHARGE;
            else if (params.battery_percent < 20) batt_sym = LV_SYMBOL_BATTERY_EMPTY;
            lv_label_set_text_fmt(self._statBattLabel, "%s %d%%", batt_sym, params.battery_percent);
        }

        bool sd_online = (HW_SD_ONLINE & hw_get_device_online());
        if (self._statSDLabel) {
            if (sd_online) {
                lv_obj_clear_flag(self._statSDLabel, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(self._statSDLabel, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (self._statUSBLabel) {
            if (hw_is_usb_msc_mounted()) {
                lv_obj_clear_flag(self._statUSBLabel, LV_OBJ_FLAG_HIDDEN);
                if (hw_is_usb_msc_writing()) {
                    lv_obj_set_style_text_color(self._statUSBLabel, lv_palette_main(LV_PALETTE_RED), 0);
                } else if (hw_is_usb_msc_reading()) {
                    lv_obj_set_style_text_color(self._statUSBLabel, lv_palette_main(LV_PALETTE_BLUE), 0);
                } else {
                    lv_obj_set_style_text_color(self._statUSBLabel, lv_palette_main(LV_PALETTE_GREEN), 0);
                }
                lv_obj_set_x(self._statUSBLabel, sd_online ? 25 : 5);
            } else {
                lv_obj_add_flag(self._statUSBLabel, LV_OBJ_FLAG_HIDDEN);
            }
        }

        user_setting_params_t settings;
        hw_get_user_setting(settings);
        if (self._statMemLabel) {
            if (settings.show_mem_usage) {
                uint32_t total, free_h;
                hw_get_heap_info(total, free_h);
                lv_label_set_text_fmt(self._statMemLabel, "M:%uK", free_h / 1024);
                lv_obj_clear_flag(self._statMemLabel, LV_OBJ_FLAG_HIDDEN);
                
                int x_offset = 5;
                if (sd_online) x_offset += 20;
                if (hw_is_usb_msc_mounted()) x_offset += 20;
                lv_obj_set_x(self._statMemLabel, x_offset);
            } else {
                lv_obj_add_flag(self._statMemLabel, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }, 1000, NULL);
}

void System::showMenu() {
    AppManager::getInstance().switchApp("MainMenu", _menuPanel);
    lv_group_set_default(_menuGroup);
    // Link all input devices to the menu group
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        lv_indev_set_group(indev, _menuGroup);
        indev = lv_indev_get_next(indev);
    }
    
    lv_tileview_set_tile_by_index(_mainScreen, 0, 0, LV_ANIM_OFF);
    enable_keyboard(); // Ensure keyboard is enabled in menu
}

void System::hideMenu() {
    lv_tileview_set_tile_by_index(_mainScreen, 0, 1, LV_ANIM_OFF);
}

bool System::isInMenu() const {
    if (!_mainScreen) return true;
    return (lv_tileview_get_tile_act(_mainScreen) == _menuPanel);
}

void System::loop() {
    AppManager::getInstance().update();
}

} // namespace core
