/**
 * @file      settings_registry.h
 * @brief     Registry of entries that the Settings app renders on its root
 *            page. Lets apps plug a config subpage (or a one-click action
 *            tile) into the Settings grid without editing ui_settings.cpp.
 *
 * Registration happens once at startup from `apps::register_all()`, in the
 * same place `make_*_app()` factories are installed. Insertion order
 * determines the on-screen order — first registered shows first.
 *
 * Entries are one of two kinds:
 *  - Subpage (`build` non-null) — a tile that opens an lv_menu subpage.
 *    The Settings app lazy-builds the page on first entry via `build`, and
 *    calls `reset` from ui_sys_exit so timers/cached LVGL pointers don't
 *    outlive the deleted menu.
 *  - Action (`on_click` non-null) — a tile that fires a click handler
 *    (e.g. "Files", "Remote", "Power Off" tear down Settings and switch to
 *    another app). Ignored for `reset`.
 *
 * Not every existing subpage has migrated yet — ui_settings.cpp still
 * hand-wires a few. Migrate incrementally; the registry entries render
 * before the hand-wired ones.
 */
#pragma once

#include <lvgl.h>

#include <vector>

namespace core {

struct SettingsEntry {
    const char *label = nullptr;   // row text (required)
    const char *icon  = nullptr;   // LV_SYMBOL_* glyph (required)

    // Subpage builder. When non-null the tile opens an lv_menu subpage and
    // this runs once on first entry. Receives the owning menu + the page
    // to populate. Take the same parameters as the existing
    // `<ns>_cfg::build_subpage()` helpers.
    void (*build)(lv_obj_t *menu, lv_obj_t *page) = nullptr;

    // Optional: called immediately after the subpage is created, so the
    // registering module can cache the page pointer (needed by subpages
    // that rebuild themselves in-place, e.g. weather after city change).
    void (*set_page)(lv_obj_t *page) = nullptr;

    // Optional: called from ui_sys_exit, before the menu is deleted, so
    // the registering module can kill timers and null cached pointers.
    void (*reset)() = nullptr;

    // Action handler. Mutually exclusive with `build` — when set, the tile
    // is clickable and dispatches this on LV_EVENT_CLICKED instead of
    // opening a subpage. Use for tiles like "Files" / "Power Off".
    void (*on_click)(lv_event_t *e) = nullptr;
};

// Install a settings tile. Typically called from `apps::register_all()`;
// do not call after the Settings app has been opened for the first time
// (the layout is computed once per open, so late additions take effect on
// the next open, not the current one).
void register_settings_entry(const SettingsEntry &e);

// Read-only view of the registry, in insertion order. Consumed by the
// Settings app.
const std::vector<SettingsEntry> &settings_entries();

// Fire every registered `reset` callback. Called from ui_sys_exit so each
// plugged-in subpage can drop its cached LVGL pointers before teardown.
void reset_settings_entries();

} // namespace core
