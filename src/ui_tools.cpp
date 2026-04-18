/**
 * @file      ui_tools.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"

static lv_group_t *msg_group = NULL;
static lv_group_t *prev_group;


lv_obj_t *ui_create_process_bar(lv_obj_t *parent, const char *title)
{
#if LVGL_VERSION_MAJOR == 8
    static lv_style_t style_bg;
    static lv_style_t style_indic;

    lv_style_init(&style_bg);
    lv_style_set_border_color(&style_bg, lv_color_black());
    lv_style_set_border_width(&style_bg, 2);
    lv_style_set_pad_all(&style_bg, 6);
    lv_style_set_radius(&style_bg, 6);
    lv_style_set_anim_time(&style_bg, 1000);
    lv_style_init(&style_indic);
    lv_style_set_bg_opa(&style_indic, LV_OPA_COVER);
    lv_style_set_bg_color(&style_indic, lv_color_black());
    lv_style_set_radius(&style_indic, 3);


    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 5);

    /*Make a gradient*/
    lv_style_set_bg_opa(&style, LV_OPA_COVER);
    static lv_grad_dsc_t grad;
    grad.dir = LV_GRAD_DIR_VER;
    grad.stops_count = 2;
    grad.stops[0].color = lv_palette_lighten(LV_PALETTE_GREY, 1);
    grad.stops[1].color = lv_palette_lighten(LV_PALETTE_GREY, 20);

    /*Shift the gradient to the bottom*/
    grad.stops[0].frac  = 128;
    grad.stops[1].frac  = 192;

    lv_style_set_bg_grad(&style, &grad);
#endif

    /*Create an object with the new style*/
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
#if LVGL_VERSION_MAJOR == 8
    lv_obj_add_style(cont, &style, 0);
#endif
    lv_obj_center(cont);

    lv_obj_t *bar = lv_bar_create(cont);
#if LVGL_VERSION_MAJOR == 8
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &style_bg, 0);
    lv_obj_add_style(bar, &style_indic, LV_PART_INDICATOR);
#endif
    lv_obj_set_size(bar, 200, 20);
    lv_obj_center(bar);
    lv_obj_set_user_data(bar, cont);
    lv_bar_set_value(bar, 0, LV_ANIM_ON);

    lv_obj_t *label = lv_label_create(cont);
    lv_label_set_text(label, title);
#if LVGL_VERSION_MAJOR == 8
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
#endif
    lv_obj_align_to(label, bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);

    return bar;
}

lv_obj_t *ui_create_option(lv_obj_t *parent, const char *title, const char *symbol_txt, lv_obj_t *(*widget_create)(lv_obj_t *parent), lv_event_cb_t btn_event_cb)
{
    lv_obj_t *cont;
    lv_obj_t *label;
    lv_obj_t *obj;
    lv_obj_t *btn;
    cont = lv_menu_cont_create(parent);

    label = lv_label_create(cont);
    lv_obj_set_width(label, lv_pct(25));

    lv_label_set_text(label, title);
    obj = widget_create(cont);
    if (symbol_txt) {
        lv_obj_set_size(obj, lv_pct(55), 40);
    } else {
        lv_obj_set_size(obj, lv_pct(65), 40);
    }
    lv_obj_set_style_outline_color(obj, lv_color_white(), LV_STATE_FOCUS_KEY);

    if (symbol_txt) {
        btn = lv_btn_create(cont);
        if (btn_event_cb) {
            lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);
        }
        lv_obj_set_size(btn, lv_pct(12), 40);
        label = lv_label_create(btn);
        lv_obj_center(label);
        lv_label_set_text(label, symbol_txt);
    }
    return cont;
}

void destroy_msgbox(lv_obj_t *msgbox)
{
#if LVGL_VERSION_MAJOR == 9
#else
    lv_obj_t *msg_btns = lv_msgbox_get_btns(msgbox);
    lv_group_focus_obj(msg_btns);
    lv_btnmatrix_set_btn_ctrl_all(msg_btns, LV_BTNMATRIX_CTRL_HIDDEN);
#endif
    lv_msgbox_close(msgbox);
    // lv_group_focus_freeze(msg_group, false);
    set_default_group(prev_group);
}

lv_obj_t *ui_popup_create(const char *title)
{
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(overlay);
    lv_obj_set_style_bg_color(overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 10, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(overlay, 6, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    if (title) {
        lv_obj_t *t = lv_label_create(overlay);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(t, lv_theme_get_font_large(t), 0);
    }
    return overlay;
}

void ui_popup_destroy(lv_obj_t *popup)
{
    if (popup) lv_obj_del(popup);
}

lv_obj_t *create_msgbox(lv_obj_t *parent, const char *title_txt,
                        const char *msg_txt, const char **btns,
                        lv_event_cb_t btns_event_cb, void *user_data)
{

    prev_group = lv_group_get_default();

    if (!msg_group) {
        msg_group = lv_group_create();
        lv_group_set_wrap(msg_group, true);
    }
    lv_group_remove_all_objs(msg_group);
    set_default_group(msg_group);

    lv_obj_t *msgbox;

    #if LVGL_VERSION_MAJOR == 9
    msgbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_text(msgbox, msg_txt);
    lv_obj_set_size(msgbox, lv_pct(90), lv_pct(60));
    lv_obj_set_style_bg_color(msgbox, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(msgbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(msgbox, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(msgbox, UI_BORDER_W, 0);
    lv_obj_set_style_radius(msgbox, UI_RADIUS, 0);
    lv_obj_set_style_text_color(msgbox, UI_COLOR_FG, 0);

    static lv_style_t msgbox_btn_focus_style;
    static bool msgbox_btn_style_inited = false;
    if (!msgbox_btn_style_inited) {
        lv_style_init(&msgbox_btn_focus_style);
        lv_style_set_border_width(&msgbox_btn_focus_style, UI_BORDER_W);
        lv_style_set_border_color(&msgbox_btn_focus_style, UI_COLOR_ACCENT);
        lv_style_set_border_side(&msgbox_btn_focus_style, LV_BORDER_SIDE_FULL);
        lv_style_set_radius(&msgbox_btn_focus_style, UI_RADIUS);
        msgbox_btn_style_inited = true;
    }

    uint32_t btn_cnt = 0;
    lv_obj_t *btn;
    lv_obj_t *first_btn = NULL;
    while (btns[btn_cnt] && btns[btn_cnt][0] != '\0') {
        btn = lv_msgbox_add_footer_button(msgbox, btns[btn_cnt]);
        lv_obj_add_event_cb(btn, btns_event_cb, LV_EVENT_CLICKED, user_data);
        lv_group_add_obj(msg_group, btn);
        lv_obj_add_style(btn, &msgbox_btn_focus_style, LV_STATE_FOCUS_KEY);
        lv_obj_add_style(btn, &msgbox_btn_focus_style, LV_STATE_FOCUSED);
        if (btn_cnt == 0) first_btn = btn;
        btn_cnt++;
    }
    if (first_btn) {
        lv_group_focus_obj(first_btn);
        lv_obj_add_state(first_btn, LV_STATE_FOCUS_KEY);
    }

#else
    msgbox = lv_msgbox_create(lv_layer_top(), title_txt, " ", btns, false);
    lv_msgbox_t *mbox = (lv_msgbox_t *)msgbox;
    lv_label_set_text_fmt(mbox->text, msg_txt);
    lv_label_set_long_mode(mbox->text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(mbox->text, lv_pct(100));

    lv_obj_t *content = lv_msgbox_get_text(msgbox);
    lv_obj_set_style_text_font(content, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(content, lv_color_white(), LV_PART_MAIN);

    lv_obj_t *msg_btns = lv_msgbox_get_btns(msgbox);
    lv_btnmatrix_set_btn_ctrl(msg_btns, 0, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_style_text_color(msg_btns, lv_color_white(), 0);

    static lv_style_t msgbox_btn_focus_style_v8;
    static bool msgbox_btn_style_v8_inited = false;
    if (!msgbox_btn_style_v8_inited) {
        lv_style_init(&msgbox_btn_focus_style_v8);
        lv_style_set_border_width(&msgbox_btn_focus_style_v8, 3);
        lv_style_set_border_color(&msgbox_btn_focus_style_v8, lv_color_white());
        lv_style_set_border_side(&msgbox_btn_focus_style_v8, LV_BORDER_SIDE_FULL);
        lv_style_set_radius(&msgbox_btn_focus_style_v8, 5);
        msgbox_btn_style_v8_inited = true;
    }
    lv_obj_add_style(msg_btns, &msgbox_btn_focus_style_v8, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(msg_btns, &msgbox_btn_focus_style_v8, LV_PART_ITEMS | LV_STATE_FOCUSED);

    lv_group_add_obj(msg_group, msg_btns);
    lv_group_focus_obj(msg_btns);

    lv_obj_set_size(msgbox, lv_pct(90), lv_pct(60));
    lv_obj_set_style_radius(msgbox, 30, LV_PART_MAIN);

    lv_obj_set_style_bg_color(msgbox, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(msgbox, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(msgbox, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(msgbox, 2, LV_PART_MAIN);

    lv_obj_t *title = lv_msgbox_get_title(msgbox);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);

    lv_obj_add_event_cb(msgbox, btns_event_cb, LV_EVENT_CLICKED, user_data);

    lv_obj_center(msgbox);
#endif

    return msgbox;

}

static void child_focus_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * parent = lv_obj_get_parent(obj);
    if(code == LV_EVENT_FOCUSED) {
        lv_obj_add_state(parent, LV_STATE_FOCUSED);
        lv_obj_add_state(parent, LV_STATE_FOCUS_KEY);
    } else if(code == LV_EVENT_DEFOCUSED) {
        lv_obj_remove_state(parent, LV_STATE_FOCUSED);
        lv_obj_remove_state(parent, LV_STATE_FOCUS_KEY);
    }
}

lv_obj_t *create_text(lv_obj_t *parent, const char *icon, const char *txt,
                      lv_menu_builder_variant_t builder_variant)
{
    lv_obj_t *obj = lv_menu_cont_create(parent);

    lv_obj_t *img = NULL;
    lv_obj_t *label = NULL;

    if (icon) {
        img = lv_img_create(obj);
        lv_img_set_src(img, icon);
    }

    if (txt) {
        label = lv_label_create(obj);
        lv_label_set_text(label, txt);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_flex_grow(label, 1);
    }

    if (builder_variant == LV_MENU_ITEM_BUILDER_VARIANT_2 && icon && txt) {
        lv_obj_add_flag(img, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        lv_obj_swap(img, label);
    }

    return obj;
}

lv_obj_t *create_slider(lv_obj_t *parent, const char *icon, const char *txt, int32_t min, int32_t max,
                        int32_t val, lv_event_cb_t cb, lv_event_code_t filter)
{
    lv_obj_t *obj = create_text(parent, icon, txt, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *slider = lv_slider_create(obj);
    lv_obj_set_style_outline_width(slider, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(slider, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(slider, 0, LV_STATE_FOCUSED);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, val, LV_ANIM_OFF);

    lv_obj_add_event_cb(slider, child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(slider, child_focus_cb, LV_EVENT_DEFOCUSED, NULL);

    if (cb != NULL) {
        lv_obj_add_event_cb(slider, cb, filter, NULL);
    }

    return slider;
}

lv_obj_t *create_switch(lv_obj_t *parent, const char *icon, const char *txt, bool chk, lv_event_cb_t cb)
{
    lv_obj_t *obj = create_text(parent, icon, txt, LV_MENU_ITEM_BUILDER_VARIANT_1);

    lv_obj_t *sw = lv_switch_create(obj);
    lv_obj_set_style_outline_width(sw, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(sw, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(sw, 0, LV_STATE_FOCUSED);
    lv_obj_add_state(sw, chk ? LV_STATE_CHECKED : LV_STATE_DEFAULT);
    
    lv_obj_add_event_cb(sw, child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(sw, child_focus_cb, LV_EVENT_DEFOCUSED, NULL);

    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

lv_obj_t *create_button(lv_obj_t *parent, const char *icon, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *obj = create_text(parent, icon, txt, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_obj_t *btn = lv_btn_create(obj);
    lv_obj_set_size(btn, lv_pct(10), lv_pct(100));
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

lv_obj_t *create_label(lv_obj_t *parent, const char *icon, const char *txt, const char *default_text)
{
    lv_obj_t *obj = create_text(parent, icon, txt, LV_MENU_ITEM_BUILDER_VARIANT_1);
    if (default_text) {
        lv_obj_t *label = lv_label_create(obj);
        lv_label_set_text(label, default_text);
        return label;
    }
    return obj;
}

lv_obj_t *create_dropdown(lv_obj_t *parent, const char *icon, const char *txt, const char *options, uint8_t default_sel, lv_event_cb_t cb)
{
    lv_obj_t *obj = create_text(parent, icon, txt, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_obj_t *dd = lv_dropdown_create(obj);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, default_sel);

    lv_obj_add_event_cb(dd, child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(dd, child_focus_cb, LV_EVENT_DEFOCUSED, NULL);

    if (cb) {
        lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    return dd;
}



static void float_button_event_cb(lv_event_t * e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
}

lv_obj_t *create_floating_button(lv_event_cb_t event_cb, void* user_data)
{
    lv_obj_t *float_btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(float_btn, FLOAT_BUTTON_WIDTH, FLOAT_BUTTON_HEIGHT);
    lv_obj_add_flag(float_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(float_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(float_btn, event_cb, LV_EVENT_CLICKED, user_data);
    lv_obj_set_style_radius(float_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_image_src(float_btn, LV_SYMBOL_LEFT, 0);
    lv_obj_set_style_text_font(float_btn, lv_theme_get_font_large(float_btn), 0);
    return float_btn;
}


lv_obj_t *create_radius_button(lv_obj_t *parent, const void *image, lv_event_cb_t event_cb, void* user_data)
{
    lv_obj_t *float_btn = lv_btn_create(parent);
    lv_obj_set_size(float_btn, FLOAT_BUTTON_WIDTH, FLOAT_BUTTON_HEIGHT);
    lv_obj_add_flag(float_btn, LV_OBJ_FLAG_FLOATING);
    // lv_obj_align(float_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(float_btn, event_cb, LV_EVENT_CLICKED, user_data);
    lv_obj_set_style_radius(float_btn, LV_RADIUS_CIRCLE, 0);
    if (image) {
        lv_obj_set_style_bg_image_src(float_btn, image, 0);
    }
    lv_obj_set_style_text_font(float_btn, lv_theme_get_font_large(float_btn), 0);
    return float_btn;
}

lv_obj_t *create_back_button(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

lv_obj_t *create_menu(lv_obj_t *parent, lv_event_cb_t event_cb)
{
    lv_obj_t *menu = lv_menu_create(parent);
#if LVGL_VERSION_MAJOR == 9
    lv_menu_set_mode_root_back_button(menu, LV_MENU_ROOT_BACK_BUTTON_DISABLED);
#else
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_DISABLED);
#endif
    lv_obj_add_event_cb(menu, event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(100));
    lv_obj_center(menu);
    return menu;
}

#ifndef ARDUINO
lv_indev_t *lv_get_encoder_indev()
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            return indev;
        }
        indev = lv_indev_get_next(indev);
    }
    return NULL;
}


lv_indev_t *lv_get_keyboard_indev()
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            return indev;
        }
        indev = lv_indev_get_next(indev);
    }
    return NULL;
}
#endif

void disable_input_devices()
{
    hw_disable_input_devices();
    lv_indev_enable(lv_get_encoder_indev(), false);
    if (hw_has_keyboard()) {
        lv_indev_enable(lv_get_keyboard_indev(), false);
    }
}

void enable_input_devices()
{
    hw_enable_input_devices();
    lv_indev_enable(lv_get_encoder_indev(), true);
    if (hw_has_keyboard()) {
        lv_indev_enable(lv_get_keyboard_indev(), true);
    }
}

// Tracks whether the I2C keyboard has been brought up. Re-running
// hw_enable_keyboard() without an intervening disable re-runs kb.begin() and
// re-attaches the ISR, which hangs on the TCA8418 — this flag keeps the
// enable/disable pair idempotent across asymmetric call sites (e.g. fresh
// boot → Settings, where the menu enabled it but MenuApp::onStop never ran
// the disable side).
static bool s_keyboard_enabled = false;

void disable_keyboard()
{
    if (!s_keyboard_enabled) return;
    if (hw_has_keyboard()) {
        hw_disable_keyboard();
        lv_indev_enable(lv_get_keyboard_indev(), false);
    }
    s_keyboard_enabled = false;
}

void enable_keyboard()
{
    if (s_keyboard_enabled) return;
    if (hw_has_keyboard()) {
        hw_enable_keyboard();
        lv_indev_enable(lv_get_keyboard_indev(), true);
    }
    s_keyboard_enabled = true;
}

bool is_screen_small()
{
    lv_coord_t w = lv_disp_get_hor_res(NULL);
    lv_coord_t h = lv_disp_get_ver_res(NULL);
    printf("Screen size: %dx%d\n", w, h);
    if (w <= 240 || h <= 240) {
        printf("Small screen detected.\n");
        return true;
    }
    return false;
}
LV_FONT_DECLARE(lv_font_montserrat_10);
LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_22);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_30);
LV_FONT_DECLARE(lv_font_montserrat_32);
LV_FONT_DECLARE(lv_font_unscii_8);
LV_FONT_DECLARE(lv_font_unscii_16);
LV_FONT_DECLARE(ui_font_courier_16);
LV_FONT_DECLARE(ui_font_courier_20);
LV_FONT_DECLARE(ui_font_courier_24);
LV_FONT_DECLARE(font_inter_14);
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_18);
LV_FONT_DECLARE(font_inter_20);

// idx: 0=Montserrat, 1=Unscii 8, 2=Unscii 16, 3=Courier, 4=Inter
static const lv_font_t *pick_font(uint8_t idx, uint8_t size)
{
    if (idx == 1) return &lv_font_unscii_8;
    if (idx == 2) return &lv_font_unscii_16;
    if (idx == 3) {
        if (size <= 16) return &ui_font_courier_16;
        if (size <= 20) return &ui_font_courier_20;
        return &ui_font_courier_24;
    }
    if (idx == 4) {
        if (size <= 14) return &font_inter_14;
        if (size <= 16) return &font_inter_16;
        if (size <= 18) return &font_inter_18;
        return &font_inter_20;
    }
    // Default to Montserrat
    switch (size) {
    case 10: return &lv_font_montserrat_10;
    case 12: return &lv_font_montserrat_12;
    case 14: return &lv_font_montserrat_14;
    case 16: return &lv_font_montserrat_16;
    case 18: return &lv_font_montserrat_18;
    case 20: return &lv_font_montserrat_20;
    case 22: return &lv_font_montserrat_22;
    case 24: return &lv_font_montserrat_24;
    case 26: return &lv_font_montserrat_26;
    case 28: return &lv_font_montserrat_28;
    case 30: return &lv_font_montserrat_30;
    case 32: return &lv_font_montserrat_32;
    default: return &lv_font_montserrat_14;
    }
}

const lv_font_t *get_editor_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.editor_font_index, settings.editor_font_size);
}

const lv_font_t *get_small_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);

    uint8_t idx = settings.editor_font_index;
    uint8_t size = settings.editor_font_size;

    if (idx == 2) return &lv_font_unscii_8; // Unscii 16 -> 8

    // For scalable faces, go 2 sizes smaller, min 10
    if (idx == 0 || idx == 4) {
        int s = (int)size - 2;
        if (s < 10) s = 10;
        size = (uint8_t)s;
    }
    return pick_font(idx, size);
}

const lv_font_t *get_journal_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.journal_font_index, settings.journal_font_size);
}

const lv_font_t *get_md_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.md_font_index, settings.md_font_size);
}

const lv_font_t *get_header_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.header_font_index, settings.header_font_size);
}

const lv_font_t *get_home_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.home_font_index, settings.home_font_size);
}
