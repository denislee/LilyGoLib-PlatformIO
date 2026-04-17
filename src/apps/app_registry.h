/**
 * @file      app_registry.h
 * @brief     Factory functions for every app the System should know about.
 *
 * Each ui_*.cpp file defines a `core::App` subclass and exposes it via one of
 * the `make_*` factories declared here. `register_all()` installs them into
 * `core::AppManager` — both `factory.ino` (hardware) and `main.cpp` (emulator)
 * call it during startup so the two entry points stay in sync.
 */
#pragma once

#include "../core/app.h"
#include <memory>

namespace apps {

std::shared_ptr<core::App> make_text_editor_app();
std::shared_ptr<core::App> make_tasks_app();
std::shared_ptr<core::App> make_blog_app();
std::shared_ptr<core::App> make_sys_app();
std::shared_ptr<core::App> make_file_browser_app();

void register_all();

} // namespace apps
