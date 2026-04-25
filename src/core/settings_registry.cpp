/**
 * @file      settings_registry.cpp
 */
#include "settings_registry.h"

namespace core {

namespace {
std::vector<SettingsEntry> &entries()
{
    // Function-local static so first `register_settings_entry()` call wins
    // initialization regardless of static-init order across translation
    // units. Matters because apps::register_all() can touch this before
    // main() on some toolchains.
    static std::vector<SettingsEntry> s_entries;
    return s_entries;
}
} // namespace

void register_settings_entry(const SettingsEntry &e)
{
    if (!e.label || !e.icon) return;
    // A valid entry is either a subpage (has `build`) or an action (has
    // `on_click`) — reject empty stubs silently so they don't render as
    // dead tiles.
    if (!e.build && !e.on_click) return;
    entries().push_back(e);
}

const std::vector<SettingsEntry> &settings_entries()
{
    return entries();
}

void reset_settings_entries()
{
    for (auto &e : entries()) {
        if (e.reset) e.reset();
    }
}

} // namespace core
