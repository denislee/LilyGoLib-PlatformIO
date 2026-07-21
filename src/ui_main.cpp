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

    // Return to the home menu instead of the note app — but only when no
    // passphrase modal is showing. showMenu() reassigns every indev to the
    // menu group, which would steal input from a modal that's still on
    // lv_layer_top(), leaving the user staring at a password field they
    // can't type into until they navigate the menu underneath.
    if (!ui_passphrase_is_active()) {
        core::System::getInstance().showMenu();
    }

    lv_display_trigger_activity(NULL);

    ui_pause_timers();

    lv_timer_del(t);
}

void ui_request_editor_switch()
{
    fake_sleep_active = false;
    // Wake the LVGL task now so the deferred-switch timer below runs on the
    // next cycle instead of waiting out its fake-sleep idle timeout.
    hw_lvgl_task_notify_wake();
    lv_timer_create(deferred_switch_timer_cb, 10, NULL);
}

void ui_resume_timers()
{
    fake_sleep_active = false;
    hw_power_up_all();
    enable_keyboard();
    lv_display_trigger_activity(NULL);
    // Kick the LVGL, keyboard, rotary and BLE keepalive tasks out of their
    // fake-sleep blocks so the first frame, keypress, scroll/click and BLE
    // conn-param re-apply after wake happen immediately rather than after
    // the idle-poll fallback.
    hw_lvgl_task_notify_wake();
    hw_keyboard_task_notify_wake();
    hw_rotary_task_notify_wake();
    hw_ble_kb_task_notify_wake();
}

void ui_pause_timers()
{
    fake_sleep_active = true;
    disable_keyboard();
    hw_power_down_all();
    // Fake-sleep just began — let the charge task start watching VBUS so a
    // cable plug can still surface the charging overlay.
    hw_charge_task_on_fake_sleep_enter();
}

bool ui_is_fake_sleep()
{
    return fake_sleep_active;
}

// Called from the vendor LilyGoLib radio/power paths to guard the shared SPI
// bus. New in-tree code should prefer core::ScopedInstanceLock.
void ui_lock()   { instanceLockTake(); }
void ui_unlock() { instanceLockGive(); }

void menu_show()
{
    core::System::getInstance().showMenu();
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
