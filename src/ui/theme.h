/**
 * @file      ui/theme.h
 * @brief     Color tokens, radius, border width.
 *
 * Use these in every place that picks a color instead of hard-coding —
 * a theme swap should be a single-header change, not a global grep.
 */
#pragma once

#include <lvgl.h>

#define DEFAULT_OPA          100

/* Unified theme tokens. Use these everywhere instead of hard-coded colors. */
#define UI_COLOR_ACCENT      lv_palette_main(LV_PALETTE_ORANGE)
#define UI_COLOR_MUTED       lv_palette_main(LV_PALETTE_GREY)
#define UI_COLOR_BG          lv_color_black()
#define UI_COLOR_FG          lv_color_white()
#define UI_RADIUS            8
#define UI_BORDER_W          2

void theme_init();
