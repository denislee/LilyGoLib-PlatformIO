/**
 * @file      app_registry.cpp
 */
#include "app_registry.h"
#include "../core/app_manager.h"

namespace apps {

void register_all() {
    auto &am = core::AppManager::getInstance();
    am.registerApp(make_text_editor_app());
    am.registerApp(make_tasks_app());
    am.registerApp(make_blog_app());
    am.registerApp(make_sys_app());
    am.registerApp(make_file_browser_app());
}

} // namespace apps
