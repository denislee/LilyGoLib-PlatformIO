/**
 * @file      ui_main.cpp
 *
 * Glue layer between the LilyGoLib vendor code (which calls the
 * `ui_request_editor_switch` / `ui_resume_timers` / `ui_pause_timers` /
 * `ui_is_fake_sleep` / `ui_lock` / `ui_unlock` hooks) and the new
 * `core::System` + `core::AppManager`. New code should talk to `core::`
 * directly; this file only exists to satisfy the vendor callbacks and the
 * `menu_show` / `menu_hidden` public API.
 */
#include "ui_define.h"
#include "core/system.h"
#include "core/scoped_lock.h"

static bool fake_sleep_active = false;
bool editor_auto_edit = false;

static void deferred_switch_timer_cb(lv_timer_t *t)
{
    editor_auto_edit = false;

    core::AppManager::getInstance().switchApp("Editor", app_panel);

    if (core::System::getInstance().isInMenu()) {
        menu_hidden();
    }
    lv_display_trigger_activity(NULL);

    ui_pause_timers();

    lv_timer_del(t);
}

void ui_request_editor_switch()
{
    fake_sleep_active = false;
    lv_timer_create(deferred_switch_timer_cb, 10, NULL);
}

void ui_resume_timers()
{
    fake_sleep_active = false;
    hw_power_up_all();
    enable_keyboard();
    lv_display_trigger_activity(NULL);
}

void ui_pause_timers()
{
    fake_sleep_active = true;
    disable_keyboard();
    hw_power_down_all();
}

bool ui_is_fake_sleep()
{
    return fake_sleep_active;
}

// Called from the vendor LilyGoLib radio/power paths to guard the shared SPI
// bus. New in-tree code should prefer core::ScopedInstanceLock.
void ui_lock()   { instanceLockTake(); }
void ui_unlock() { instanceLockGive(); }

bool isinMenu()
{
    return core::System::getInstance().isInMenu();
}

void menu_show()
{
    core::System::getInstance().showMenu();
    hw_feedback();
}

void menu_hidden()
{
    core::System::getInstance().hideMenu();
}

void set_default_group(lv_group_t *group)
{
    lv_group_set_default(group);
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        lv_indev_set_group(indev, group);
        indev = lv_indev_get_next(indev);
    }
}
