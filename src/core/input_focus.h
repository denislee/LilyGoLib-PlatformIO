/**
 * @file      input_focus.h
 * @brief     Tells background work whether the user is actively typing.
 *
 * Long-running LVGL timers (network polls, status-bar FFat walk, autosave)
 * run on the LVGL task under the instance mutex. The physical-keyboard
 * reader task also takes that mutex to drain the TCA8418, so slow work
 * there delays key events. Callbacks that do I/O should bail out early
 * while a textarea is focused so typing stays responsive.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus

#include <lvgl.h>

namespace core {

// True when any keypad input device's default-focused object is an
// LVGL textarea. Safe to call from any LVGL timer callback.
bool isTextInputFocused();

} // namespace core

extern "C" {
#endif

bool ui_text_input_focused(void);

#ifdef __cplusplus
}
#endif
