/**
 * @file      ui/back_button.h
 * @brief     Shared back-button slot on the top status bar.
 *
 * Only one back-button exists; ui_show_back_button() replaces the previous
 * callback and adds the button to the current default lv_group so it stays
 * keyboard-navigable. Modal overlays save the previous callback via
 * ui_get_back_button_cb() before installing their own, then restore on
 * teardown.
 */
#pragma once

#include <lvgl.h>

lv_obj_t *ui_show_back_button(lv_event_cb_t cb);
void ui_hide_back_button(void);
lv_event_cb_t ui_get_back_button_cb(void);
