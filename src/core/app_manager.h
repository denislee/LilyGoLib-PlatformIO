/**
 * @file      app_manager.h
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "app.h"
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace core {

/**
 * @brief Manages the lifecycle of apps and switching between them.
 */
class AppManager {
public:
    static AppManager& getInstance();

    /**
     * @brief Register a new app.
     * @param app The app to register.
     */
    void registerApp(std::shared_ptr<App> app);

    /**
     * @brief Switch to an app by its name.
     * @param name The name of the app.
     * @param parent The parent object to create the UI on.
     */
    void switchApp(const std::string& name, lv_obj_t* parent);

    /**
     * @brief Get the currently active app.
     */
    std::shared_ptr<App> getActiveApp() const { return _activeApp; }

    /**
     * @brief Periodically call to update the active app.
     */
    void update();

    /**
     * @brief Get a list of all registered apps.
     */
    const std::vector<std::shared_ptr<App>>& getApps() const { return _apps; }

private:
    AppManager() = default;
    AppManager(const AppManager&) = delete;
    AppManager& operator=(const AppManager&) = delete;

    std::map<std::string, std::shared_ptr<App>> _appMap;
    std::vector<std::shared_ptr<App>> _apps;
    std::shared_ptr<App> _activeApp;
};

} // namespace core
