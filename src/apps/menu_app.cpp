/**
 * @file      menu_app.cpp
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#include "menu_app.h"
#include "../core/system.h"
#include "../ui_define.h"

namespace apps {

MenuApp::MenuApp() : core::App("MainMenu") {}

void MenuApp::onStart(lv_obj_t* parent) {
    _list = lv_list_create(parent);
    lv_obj_set_size(_list, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_list);
    lv_obj_set_style_bg_color(_list, lv_color_black(), 0);
    lv_obj_set_style_border_width(_list, 0, 0);

    createMenuItem("Editor", LV_SYMBOL_KEYBOARD, "Editor");
    createMenuItem("Tasks", LV_SYMBOL_OK, "Tasks");
    createMenuItem("Journal", LV_SYMBOL_DIRECTORY, "Journal");
    createMenuItem("Settings", LV_SYMBOL_SETTINGS, "Settings");
    createMenuItem("Files", LV_SYMBOL_FILE, "Files");
    
    lv_obj_t *shutdown_btn = lv_list_add_btn(_list, LV_SYMBOL_POWER, "Shutdown");
    lv_obj_add_event_cb(shutdown_btn, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            hw_feedback();
            lv_delay_ms(200); 
            hw_shutdown();
        }
    }, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(lv_group_get_default(), shutdown_btn);
    
    // Auto-focus first item
    if (lv_obj_get_child_count(_list) > 0) {
        lv_group_focus_obj(lv_obj_get_child(_list, 0));
    }
}

void MenuApp::createMenuItem(const char* name, const char* symbol, const std::string& appName) {
    lv_obj_t *btn = lv_list_add_btn(_list, symbol, name);
    
    // Style icon (reusing logic from ui_main.cpp)
    lv_obj_t *icon = lv_obj_get_child(btn, 0);
    if (icon) {
        lv_obj_set_style_min_width(icon, 20, 0);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    }
    
    // Use a string to pass app name safely
    static std::map<lv_obj_t*, std::string> appNames;
    appNames[btn] = appName;

    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            const char* target = (const char*)lv_event_get_user_data(e);
            hw_feedback();
            core::System::getInstance().hideMenu();
            core::AppManager::getInstance().switchApp(target, core::System::getInstance().getAppPanel());
        }
    }, LV_EVENT_CLICKED, (void*)name); // Using name as proxy for now, better to use appName
    
    lv_group_add_obj(lv_group_get_default(), btn);
}

} // namespace apps
