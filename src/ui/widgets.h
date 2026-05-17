/**
 * @file      ui/widgets.h
 * @brief     Themed LVGL widget factories.
 *
 * These wrap raw LVGL constructors with the project's theme (accent
 * border, font, padding). Use them instead of lv_*_create() so a theme
 * tweak doesn't require touching every screen.
 *
 * Implementations live in src/ui_tools.cpp.
 */
#pragma once

#include <lvgl.h>
#include <stdint.h>

/* Menu-row layout selector for create_text().
 * V1 = single-line text row; V2 = two-line row with subtitle, used by the
 * settings pages and action buttons that need an explanatory second line. */
enum class lv_menu_builder_variant_t : uint8_t {
    V1 = 0,
    V2 = 1,
};
constexpr lv_menu_builder_variant_t LV_MENU_ITEM_BUILDER_VARIANT_1 = lv_menu_builder_variant_t::V1;
constexpr lv_menu_builder_variant_t LV_MENU_ITEM_BUILDER_VARIANT_2 = lv_menu_builder_variant_t::V2;

lv_obj_t *ui_create_option(lv_obj_t *parent, const char *title, const char *symbol_txt, lv_obj_t *(*widget_create)(lv_obj_t *parent), lv_event_cb_t btn_event_cb);
lv_obj_t *create_text(lv_obj_t *parent, const char *icon, const char *txt,
                      lv_menu_builder_variant_t builder_variant);
lv_obj_t *create_slider(lv_obj_t *parent, const char *icon, const char *txt, int32_t min, int32_t max,
                        int32_t val, lv_event_cb_t cb, lv_event_code_t filter);
lv_obj_t *create_switch(lv_obj_t *parent, const char *icon, const char *txt, bool chk, lv_event_cb_t cb);
lv_obj_t *create_button(lv_obj_t *parent, const char *icon, const char *txt, lv_event_cb_t cb);
lv_obj_t *create_label(lv_obj_t *parent, const char *icon, const char *txt, const char *default_text);
lv_obj_t *create_dropdown(lv_obj_t *parent, const char *icon, const char *txt, const char *options, uint8_t default_sel, lv_event_cb_t cb);

lv_obj_t *ui_create_process_bar(lv_obj_t *parent, const char *title);

lv_obj_t *create_floating_button(lv_event_cb_t event_cb, void *user_data);
lv_obj_t *create_menu(lv_obj_t *parent, lv_event_cb_t event_cb);
lv_obj_t *create_radius_button(lv_obj_t *parent, const void *image, lv_event_cb_t event_cb, void *user_data);
