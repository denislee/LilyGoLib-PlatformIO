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
app_t *current_app_ptr = nullptr;

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

    // Create Main Screen (TileView)
    _mainScreen = lv_tileview_create(lv_screen_active());
    main_screen = _mainScreen; // Set global pointer
    lv_obj_set_size(_mainScreen, LV_PCT(100), v_res - 30);
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
    lv_obj_set_size(_statusBar, LV_PCT(100), 30);
    lv_obj_align(_statusBar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_statusBar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_statusBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_statusBar, 0, 0);
    lv_obj_set_style_radius(_statusBar, 0, 0);

    _statTimeLabel = lv_label_create(_statusBar);
    lv_obj_center(_statTimeLabel);
    lv_obj_set_style_text_color(_statTimeLabel, lv_color_white(), 0);

    _statBattLabel = lv_label_create(_statusBar);
    lv_obj_align(_statBattLabel, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_text_color(_statBattLabel, lv_color_white(), 0);

    _statMemLabel = lv_label_create(_statusBar);
    lv_obj_align(_statMemLabel, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_text_color(_statMemLabel, lv_color_white(), 0);
    lv_obj_add_flag(_statMemLabel, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create([](lv_timer_t *t) {
        System& self = System::getInstance();
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
