/**
 * @file      system_hooks.h
 * @brief     Canonical declarations for cross-module system entry points.
 *
 * Every TU that reaches across module boundaries (`ui_is_fake_sleep`,
 * `menu_show`, the instance lock, editor deep-link glue) goes through this
 * header. Replaces ad-hoc `extern bool ui_is_fake_sleep();` declarations
 * that previously drifted across factory.ino, hal/lvgl_task.cpp, and
 * hal/nfc_task.cpp — a rename used to be a landmine, now it's `grep`-able
 * from one place.
 *
 * Kept intentionally free of LVGL and vendor headers so it's cheap to
 * include from HAL / task TUs that have no UI dependency.
 */
#pragma once

#include "scoped_lock.h"

namespace core {

// One-shot initializer for the instance-lock mutex. Must run before any
// instanceLockTake() call on hardware. The emulator build is a no-op.
void instance_lock_init();

} // namespace core

// Display/power idle flag. `true` while the backlight is off and the UI
// is suspended (tasks skip expensive work, loop() drops CPU freq). Set
// via ui_pause_timers() / ui_resume_timers() in ui_main.cpp.
bool ui_is_fake_sleep();
void ui_pause_timers();
void ui_resume_timers();

// Vendor-compat aliases for the instance lock. New in-tree code should
// prefer core::ScopedInstanceLock directly.
void ui_lock();
void ui_unlock();

// Editor deep-link: used by NFC / shortcuts to jump to the text editor
// from outside the editor itself.
void ui_request_editor_switch();
extern bool editor_auto_edit;

// Menu panel visibility. Thin wrappers over core::System.
bool isinMenu();
void menu_show();
void menu_hidden();
