/**
 * @file      ui.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-04
 *
 */
#include "ui_define.h"
#include "core/system.h"

// Global UI objects (main_screen, menu_panel, app_panel, groups) live in core/system.cpp.
// `disp_timer` is kept as a null handle for legacy null-safe callers in this file.
static lv_timer_t *disp_timer = nullptr;
static bool low_power_mode_flag = false;
bool editor_auto_edit = false;
static bool fake_sleep_active = false;

static void deferred_switch_timer_cb(lv_timer_t *t)
{
    // Set flag so editor knows whether to enter edit mode or not
    editor_auto_edit = false;

    // Use the AppManager to handle the switch safely. 
    // This will call onStop() on the current app and onStart() on the Editor.
    core::AppManager::getInstance().switchApp("Editor", app_panel);

    if (isinMenu()) {
        menu_hidden();
    }
    lv_display_trigger_activity(NULL);
    
    // Switch is done, now we can go back to "quiet" sleep mode
    ui_pause_timers();

    lv_timer_del(t);
}

void ui_request_editor_switch()
{
    // Make sure timers are active to process this request
    fake_sleep_active = false;
    if (disp_timer) {
        lv_timer_resume(disp_timer);
    }
    // Start a one-shot timer to perform the switch in the next safe loop cycle
    lv_timer_create(deferred_switch_timer_cb, 10, NULL);
}

void ui_resume_timers()
{
    fake_sleep_active = false;
    if (disp_timer) {
        lv_timer_resume(disp_timer);
    }
    hw_power_up_all();
    enable_keyboard();
    lv_display_trigger_activity(NULL);
}

void ui_pause_timers()
{
    fake_sleep_active = true;
    if (disp_timer) {
        lv_timer_pause(disp_timer);
    }
    disable_keyboard();
    hw_power_down_all();
}

bool ui_is_fake_sleep()
{
    return fake_sleep_active;
}

extern void instanceLockTake();
extern void instanceLockGive();

void ui_lock()
{
    instanceLockTake();
}

void ui_unlock()
{
    instanceLockGive();
}

void set_low_power_mode_flag(bool enable)
{
    low_power_mode_flag = enable;
}

void menu_show()
{
    core::System::getInstance().showMenu();
    if (disp_timer) {
        lv_timer_resume(disp_timer);
    }
    hw_feedback();
}

void menu_hidden()
{
    core::System::getInstance().hideMenu();
}

bool isinMenu()
{
    if (!main_screen) return true;
    return (lv_tileview_get_tile_act(main_screen) == menu_panel);
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

