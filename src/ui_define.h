/**
 * @file      ui_define.h
 * @brief     Back-compat aggregator. Prefer the focused ui/* headers in
 *            new code:
 *              ui/widgets.h     — themed lv_*_create wrappers
 *              ui/theme.h       — color tokens, radius, theme_init
 *              ui/fonts.h       — per-context font getters
 *              ui/modals.h      — popups, message boxes, loading, prompts
 *              ui/back_button.h — shared top-bar back button
 *
 * This header re-exports all of them plus the LVGL/Arduino/LilyGoLib
 * transitive bag that the older code relied on. It also still owns the
 * extern handles to the global panels and a few cross-module hooks that
 * don't have a better home yet.
 */
#ifdef ARDUINO
#include <Arduino.h>
#include <LilyGoLib.h>
/* WiFi.h arrives transitively via hal/types.h on both platforms.
 * esp_mac.h was unused here — moved to hal/system.cpp where it belongs. */
#endif
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include "hal_interface.h"
#include "core/system_hooks.h"

#include "ui/theme.h"
#include "ui/fonts.h"
#include "ui/widgets.h"
#include "ui/modals.h"
#include "ui/back_button.h"

/* Cross-module globals + hooks that don't belong in a feature header. */
extern lv_group_t *menu_g;

lv_indev_t *lv_get_encoder_indev();
lv_indev_t *lv_get_keyboard_indev();
/* menu_show / menu_hidden declared in core/system_hooks.h (via include above). */
void set_default_group(lv_group_t *group);

void disable_keyboard();
void enable_keyboard();

void ui_text_editor_open_file(const char *path);

/* Editor / fake-sleep / instance-lock hooks are declared in
 * core/system_hooks.h (included at top of this file). */

/* LVGL v8->v9 rename shims. This tree builds only against LVGL 9.x, so the
 * old-name aliases below let pre-existing code keep compiling unchanged. */
#define LV_MENU_ROOT_BACK_BTN_ENABLED   LV_MENU_ROOT_BACK_BUTTON_ENABLED
#define lv_menu_back_btn_is_root        lv_menu_back_button_is_root
#define lv_menu_set_mode_root_back_btn  lv_menu_set_mode_root_back_button
#define lv_mem_alloc                    lv_malloc
#define lv_mem_free                     lv_free
#define LV_IMG_CF_ALPHA_8BIT            LV_COLOR_FORMAT_L8
#define lv_point_t                      lv_point_precise_t

#ifndef M_PI
#define M_PI		3.14159265358979323846
#endif
