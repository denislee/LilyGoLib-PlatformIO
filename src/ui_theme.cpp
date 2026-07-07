/**
 * @file      ui_theme.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-04-23
 * 
 */

#include "ui_define.h"

/*Will be called when the styles of the base theme are already added
  to add new styles*/
static void new_theme_apply_cb(lv_theme_t * th, lv_obj_t * obj)
{
    static lv_style_t black_bg_style;
    static bool inited = false;
    if (!inited) {
        lv_style_init(&black_bg_style);
        lv_style_set_bg_color(&black_bg_style, lv_color_black());
        lv_style_set_text_color(&black_bg_style, lv_color_white());
        lv_style_set_bg_opa(&black_bg_style, LV_OPA_COVER);
        lv_style_set_border_width(&black_bg_style, 0);
        inited = true;
    }

    if (lv_obj_check_type(obj, &lv_button_class) ||
        lv_obj_check_type(obj, &lv_list_button_class) ||
        lv_obj_check_type(obj, &lv_menu_cont_class) ||
        lv_obj_check_type(obj, &lv_slider_class) ||
        lv_obj_check_type(obj, &lv_switch_class) ||
        lv_obj_check_type(obj, &lv_spinbox_class) ||
        lv_obj_check_type(obj, &lv_textarea_class) ||
        lv_obj_check_type(obj, &lv_dropdown_class) ||
        lv_obj_check_type(obj, &lv_checkbox_class)) {

        // Reserve border space in the default state (transparent) so that
        // toggling focus only swaps the border color — the object's layout
        // size never changes and sibling list items don't shift.
        static lv_style_t default_border_style;
        static lv_style_t focus_style;
        static bool focus_inited = false;
        if (!focus_inited) {
            lv_style_init(&default_border_style);
            lv_style_set_border_width(&default_border_style, UI_BORDER_W);
            lv_style_set_border_opa(&default_border_style, LV_OPA_TRANSP);

            lv_style_init(&focus_style);
            lv_style_set_border_width(&focus_style, UI_BORDER_W);
            lv_style_set_border_color(&focus_style, UI_COLOR_ACCENT);
            lv_style_set_border_opa(&focus_style, LV_OPA_COVER);
            lv_style_set_outline_width(&focus_style, 0);
            focus_inited = true;
        }
        lv_obj_add_style(obj, &default_border_style, LV_PART_MAIN);
        lv_obj_add_style(obj, &focus_style, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &focus_style, LV_STATE_FOCUS_KEY);
    }

    if (lv_obj_check_type(obj, &lv_list_class) || 
        lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, &black_bg_style, LV_PART_MAIN);
    }
}

void theme_init()
{
    lv_display_t * disp = lv_display_get_default();
    const lv_font_t *font = get_system_font();
    if (!font) font = MAIN_FONT;
    lv_theme_t * th = lv_theme_default_init(disp,
                                            lv_color_black(),
                                            lv_palette_main(LV_PALETTE_GREY),
                                            true,
                                            font);
    lv_theme_set_apply_cb(th, new_theme_apply_cb);
    lv_display_set_theme(disp, th);
}


