/**
 * @file      menu_app.h
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "../core/app.h"

namespace apps {

class MenuApp : public core::App {
public:
    MenuApp();
    void onStart(lv_obj_t* parent) override;
    void onStop() override;
};

} // namespace apps
