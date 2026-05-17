/**
 * @file      menu_glance.h
 * @brief     Home-screen "glance" overlay — full-screen at-a-glance readout
 *            (clock, date, battery, connectivity, telegram unread count)
 *            in a bento grid, rotated 180° for an upside-down read.
 *
 * Extracted from menu_app.cpp so the home screen's main layout code isn't
 * interleaved with the overlay's ~250 lines of layout + refresh logic.
 * The overlay is self-contained — it owns its own lv_group_t, lv_timer_t,
 * and overlay obj, restoring the previous default group on dismiss. Safe
 * to call glance_show() while the main menu is up; dismissed by any tap
 * or key.
 */
#pragma once

namespace apps {
namespace menu {

void glance_show();

} // namespace menu
} // namespace apps
