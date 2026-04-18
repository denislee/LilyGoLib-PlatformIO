/**
 * @file      ui_power.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"

static lv_obj_t *menu = NULL;

void ui_power_exit(lv_obj_t *parent);

static void event_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (obj == NULL) return;

    const char *text = lv_label_get_text(lv_obj_get_child(obj, 0));
    printf("Button %s clicked\n", text);
    if (strcmp(text, "Deep Sleep") == 0) {
        lv_obj_clean(lv_screen_active());
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_radius(lv_screen_active(), 0, 0);

        LV_IMG_DECLARE(img_poweroff);
        lv_obj_t *image = lv_image_create(lv_screen_active());
        lv_image_set_src(image, &img_poweroff);
        lv_obj_center(image);

        lv_obj_t *label = lv_label_create(lv_screen_active());
        lv_label_set_text(label, "Entering Deep Sleep...");
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -30);

        lv_refr_now(NULL);
        lv_delay_ms(2000);
        
        hw_sleep();

    } else if (strcmp(text, "Shutdown") == 0) {
        lv_obj_clean(lv_screen_active());
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_radius(lv_screen_active(), 0, 0);

        LV_IMG_DECLARE(img_poweroff);
        lv_obj_t *image = lv_image_create(lv_screen_active());
        lv_image_set_src(image, &img_poweroff);
        lv_obj_center(image);

        lv_obj_t *label = lv_label_create(lv_screen_active());
        lv_label_set_text(label, "Power Off...");
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -30);

        lv_refr_now(NULL);
        lv_delay_ms(3000);
        hw_shutdown();

    } else if (strcmp(text, "Sleep") == 0) {
        hw_light_sleep();
    } else if (strcmp(text, "Close") == 0) {
        ui_power_exit(NULL);
        menu_show();
    }
}

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        ui_power_exit(NULL);
        menu_show();
    }
}

static void power_back_cb(lv_event_t *e)
{
    if (!menu) return;
    lv_obj_t *bb = lv_menu_get_main_header_back_button(menu);
    if (bb) lv_obj_send_event(bb, LV_EVENT_CLICKED, NULL);
}

void ui_power_enter(lv_obj_t *parent)
{
    bool is_small = is_screen_small();
    uint16_t btn_w = is_small ? 75 : 120;
    uint16_t btn_h = is_small ? 30 : 40;

    menu = create_menu(parent, back_event_handler);
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_ENABLED);

    // Suppress the built-in header back button by zero-sizing it so LVGL
    // auto-hides the header (content_height becomes 0). Still clickable
    // programmatically via send_event from the status bar back.
    lv_obj_t *bb = lv_menu_get_main_header_back_button(menu);
    if (bb) {
        lv_obj_set_size(bb, 0, 0);
        lv_obj_set_style_pad_all(bb, 0, 0);
        lv_obj_set_style_border_width(bb, 0, 0);
        lv_obj_set_style_outline_width(bb, 0, 0);
        lv_obj_set_style_shadow_width(bb, 0, 0);
        lv_obj_set_style_bg_opa(bb, LV_OPA_TRANSP, 0);
    }
    ui_show_back_button(power_back_cb);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_scrollbar_mode(main_page, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *label = lv_label_create(main_page);
    lv_label_set_text(label, hw_get_device_power_tips_string());
    if (is_small) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    } else {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    }
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_width(label, lv_pct(90));
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btns_cont = lv_obj_create(main_page);
    lv_obj_set_scroll_dir(btns_cont, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(btns_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(btns_cont, lv_pct(99), 100);
    lv_obj_set_style_margin_top(btns_cont, is_small ? 15 : 80, 0);
    lv_obj_set_style_border_width(btns_cont, 0, 0);
    lv_obj_set_flex_flow(btns_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(btns_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);

    lv_obj_t *btn_deep_sleep = lv_btn_create(btns_cont);
    lv_obj_set_size(btn_deep_sleep, btn_w, btn_h);
    lv_obj_t *label_btn = lv_label_create(btn_deep_sleep);
    lv_label_set_text(label_btn, "Deep Sleep");
    lv_obj_center(label_btn);
    lv_obj_add_event_cb(btn_deep_sleep, event_cb, LV_EVENT_CLICKED, NULL);
    if (is_small) {
        lv_obj_set_style_text_font(label_btn, &lv_font_montserrat_14, 0);
    } else {
        lv_obj_set_style_text_font(label_btn, &lv_font_montserrat_24, 0);
    }

    lv_obj_t *btn_shutdown = lv_btn_create(btns_cont);
    lv_obj_set_size(btn_shutdown, btn_w, btn_h);
    label_btn = lv_label_create(btn_shutdown);
    lv_label_set_text(label_btn, "Shutdown");
    lv_obj_center(label_btn);
    lv_obj_add_event_cb(btn_shutdown, event_cb, LV_EVENT_CLICKED, NULL);
    if (is_small) {
        lv_obj_set_style_text_font(label_btn, &lv_font_montserrat_14, 0);
    } else {
        lv_obj_set_style_text_font(label_btn, &lv_font_montserrat_24, 0);
    }


    lv_obj_t *btn_sleep = lv_btn_create(btns_cont);
    lv_obj_set_size(btn_sleep, btn_w, btn_h);
    label_btn = lv_label_create(btn_sleep);
    lv_label_set_text(label_btn, "Sleep");
    lv_obj_center(label_btn);
    lv_obj_add_event_cb(btn_sleep, event_cb, LV_EVENT_CLICKED, NULL);
    if (is_small) {
        lv_obj_set_style_text_font(label_btn, &lv_font_montserrat_14, 0);
    } else {
        lv_obj_set_style_text_font(label_btn, &lv_font_montserrat_24, 0);
    }

    lv_obj_t *quit_btn = lv_btn_create(btns_cont);
    lv_obj_set_size(quit_btn, btn_w, btn_h);
    label_btn = lv_label_create(quit_btn);
    lv_label_set_text(label_btn, "Close");
    lv_obj_center(label_btn);
    lv_obj_add_event_cb(quit_btn, event_cb, LV_EVENT_CLICKED, NULL);
    if (is_small) {
        lv_obj_set_style_text_font(label_btn, &lv_font_montserrat_14, 0);
    } else {
        lv_obj_set_style_text_font(label_btn, &lv_font_montserrat_24, 0);
    }

    lv_menu_set_page(menu, main_page);
}

void ui_power_exit(lv_obj_t *parent)
{
    ui_hide_back_button();
    if (menu) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
    }
}



