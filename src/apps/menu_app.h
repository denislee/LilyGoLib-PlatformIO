/**
 * @file      menu_app.h
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "../core/app.h"
#include <vector>

namespace apps {

class MenuApp : public core::App {
public:
    MenuApp();
    void onStart(lv_obj_t* parent) override;
    
private:
    lv_obj_t* _list = nullptr;
    
    void createMenuItem(const char* name, const char* symbol, const std::string& appName);
    static void btn_event_cb(lv_event_t* e);
};

} // namespace apps
