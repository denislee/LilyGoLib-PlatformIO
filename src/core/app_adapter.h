/**
 * @file      app_adapter.h
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "app.h"
#include "../ui_define.h"

namespace core {

/**
 * @brief Adapter to allow existing C-style app_t structures to work with the App interface.
 */
class AppAdapter : public App {
public:
    AppAdapter(const std::string& name, app_t* legacy_app) 
        : App(name), _legacyApp(legacy_app) {}

    void onStart(lv_obj_t* parent) override {
        current_app_ptr = _legacyApp;
        if (_legacyApp && _legacyApp->setup_func_cb) {
            _legacyApp->setup_func_cb(parent);
        }
    }

    void onStop() override {
        if (_legacyApp && _legacyApp->exit_func_cb) {
            _legacyApp->exit_func_cb(_root);
        }
        if (current_app_ptr == _legacyApp) {
            current_app_ptr = nullptr;
        }
        App::onStop();
    }

private:
    app_t* _legacyApp;
};

} // namespace core
