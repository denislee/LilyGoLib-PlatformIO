/**
 * @file      ui_list_picker.h
 * @brief     Modal list-picker overlay.
 *
 * Pops a full-height overlay over the current screen with a scrollable list
 * of labelled items. The user picks one with the encoder/keyboard/touch; the
 * callback fires with the picked index (or -1 on cancel). Used by settings
 * flows that surface choices returned from a network call (weather city
 * search, timezone list, etc.).
 */
#pragma once

#include <string>
#include <vector>

typedef void (*ui_list_picker_cb)(int index, void *ud);

// `items` is copied before the call returns, so the caller can free its copy
// immediately. `cb` fires exactly once with the picked index or -1 if the
// user dismissed the picker via the back button.
void ui_list_picker_open(const char *title,
                         const std::vector<std::string> &items,
                         ui_list_picker_cb cb, void *ud);
