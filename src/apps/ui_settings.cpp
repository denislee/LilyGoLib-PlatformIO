/**
 * @file      ui_settings.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "../ui_define.h"
#include "../hal/notes_crypto.h"
#include "../core/app_manager.h"
#include "../core/system.h"
#include "app_registry.h"
#include "../ui_list_picker.h"
#include "settings_internal.h"
#include <vector>

using std::string;
using std::vector;

// weather_city_match / weather_*  and  timezone_*  forward decls all live
// in apps/settings_internal.h (shared with the split-out settings_*.cpp).

static void style_menu_item_icon(lv_obj_t *cont, const char *icon, const char *text)
{
    lv_obj_t *icon_label = lv_label_create(cont);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_min_width(icon_label, 20, 0);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *text_label = lv_label_create(cont);
    lv_label_set_text(text_label, text);
}

static lv_obj_t *menu = NULL;
// External linkage — shared with the split-out settings_*.cpp files via
// apps/settings_internal.h. Not strictly a global API; treat as private.
user_setting_params_t local_param;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *settings_main_page = NULL;
static lv_obj_t *settings_exit_btn = NULL;

void ui_sys_exit(lv_obj_t *parent);

#define MAX_MAIN_PAGE_ITEMS 16
static lv_obj_t *main_page_group_items[MAX_MAIN_PAGE_ITEMS];
static uint8_t main_page_group_count = 0;

#define MAX_SUBPAGE_ITEMS 64
static struct {
    lv_obj_t *page;
    lv_obj_t *obj;
} subpage_items[MAX_SUBPAGE_ITEMS];
static uint8_t subpage_item_count = 0;

static void add_main_page_group_item(lv_obj_t *obj)
{
    if (main_page_group_count < MAX_MAIN_PAGE_ITEMS) {
        main_page_group_items[main_page_group_count++] = obj;
    }
    lv_group_add_obj(menu_g, obj);
}

// External linkage — called from the split-out settings_*.cpp files. See
// apps/settings_internal.h.
void register_subpage_group_obj(lv_obj_t *page, lv_obj_t *obj)
{
    if (subpage_item_count < MAX_SUBPAGE_ITEMS) {
        subpage_items[subpage_item_count].page = page;
        subpage_items[subpage_item_count].obj = obj;
        subpage_item_count++;
    }
}

// External linkage — swap-remove any entries tracking widgets on `page`.
// Used by notes_sec_cfg::mark_for_rebuild before lv_obj_clean, so
// activate_subpage_group won't re-add freed widgets on the next focus.
void unregister_subpage_items_for(lv_obj_t *page)
{
    for (uint8_t i = 0; i < subpage_item_count; ) {
        if (subpage_items[i].page == page) {
            subpage_items[i] = subpage_items[--subpage_item_count];
        } else {
            i++;
        }
    }
}

static void restore_main_page_group()
{
    lv_group_remove_all_objs(menu_g);
    if (settings_exit_btn) {
        lv_group_add_obj(menu_g, settings_exit_btn);
    }
    for (uint8_t i = 0; i < main_page_group_count; i++) {
        lv_group_add_obj(menu_g, main_page_group_items[i]);
    }
    if (settings_exit_btn) {
        lv_group_focus_obj(settings_exit_btn);
    }
}

// External linkage — shared with the split-out settings_*.cpp files.
void activate_subpage_group(lv_obj_t *page)
{
    lv_group_remove_all_objs(menu_g);
    // Status bar back is the visible "<" on subpages — its callback pops to
    // root. Keep it keyboard-navigable and initially focused.
    if (settings_exit_btn) {
        lv_group_add_obj(menu_g, settings_exit_btn);
    }
    for (uint8_t i = 0; i < subpage_item_count; i++) {
        if (subpage_items[i].page == page &&
            !lv_obj_has_flag(subpage_items[i].obj, LV_OBJ_FLAG_HIDDEN)) {
            lv_group_add_obj(menu_g, subpage_items[i].obj);
        }
    }
    if (settings_exit_btn) {
        lv_group_focus_obj(settings_exit_btn);
    }
}

/* Forward decl for the external-linkage definition below — declaration
 * also appears in apps/settings_internal.h so split-out files can call it. */
lv_obj_t *create_toggle_btn_row(lv_obj_t *parent, const char *txt, bool initial_state, lv_event_cb_t cb);


// External linkage — shared with the split-out settings_*.cpp files.
void invert_scroll_key_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;
    uint32_t *key = (uint32_t *)lv_event_get_param(e);
    if (!key) return;
    switch (*key) {
    case LV_KEY_LEFT:  *key = LV_KEY_RIGHT; break;
    case LV_KEY_RIGHT: *key = LV_KEY_LEFT;  break;
    }
}

// Helper used by split-out subpages (e.g. datetime's "Apply") that need to
// pop the user back to the Settings root. Declared in apps/settings_internal.h.
void settings_return_to_main_page()
{
    if (menu && settings_main_page) {
        lv_menu_clear_history(menu);
        lv_menu_set_page(menu, settings_main_page);
    }
}

static lv_obj_t *create_subpage_datetime(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_REFRESH, "Date & Time");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&datetime_cfg::build_subpage);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static lv_obj_t *create_subpage_backlight(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_IMAGE, "Display & Backlight");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&display_cfg::build_backlight);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static lv_obj_t *create_subpage_otg(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_CHARGE, "Charger");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&display_cfg::build_otg);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static lv_obj_t *create_subpage_performance(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_SETTINGS, "Performance");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&display_cfg::build_performance);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}


static lv_obj_t *create_subpage_info(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_LIST, "System Info");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&info_cfg::build_subpage);
    lv_menu_set_load_page_event(menu, cont, sub_page);

    return cont;
}

static void build_device_probe(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_add_flag(sub_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sub_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sub_page, 4, 0);
    lv_obj_set_style_pad_row(sub_page, 0, 0);

    uint8_t devices = hw_get_devices_nums();
    uint32_t devices_mask = hw_get_device_online();
    for (int i = 0; i < devices; ++i) {
        const char *device_name = hw_get_devices_name(i);
        if (lv_strcmp(device_name, "") != 0) {
            bool online = (devices_mask & 0x01);

            lv_obj_t *row = lv_menu_cont_create(sub_page);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_ver(row, 5, 0);
            lv_obj_set_style_pad_hor(row, 8, 0);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(row, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);

            lv_obj_t *name = lv_label_create(row);
            lv_label_set_text(name, device_name);
            lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL);
            lv_obj_set_style_max_width(name, LV_PCT(65), 0);

            lv_obj_t *status = lv_label_create(row);
            if (online) {
                lv_label_set_text(status, LV_SYMBOL_OK);
                lv_obj_set_style_text_color(status, lv_palette_main(LV_PALETTE_GREEN), 0);
            } else {
                lv_label_set_text(status, LV_SYMBOL_CLOSE);
                lv_obj_set_style_text_color(status, lv_palette_main(LV_PALETTE_RED), 0);
            }

            register_subpage_group_obj(sub_page, row);
        }
        devices_mask >>= 1;
    }
}

static lv_obj_t *create_device_probe(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_USB, "Devices");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)build_device_probe);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static void files_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hw_feedback();
    // Defer the app switch: ui_sys_exit() deletes the settings `menu`, which
    // is an ancestor of the Files menu item whose CLICKED event is still
    // being dispatched. Freeing it synchronously leaves LVGL walking freed
    // memory and freezes the device. queueSwitchApp runs from
    // AppManager::update() after lv_timer_handler fully unwinds — guaranteed
    // next-main-loop-iteration, unlike lv_async_call (period-0 timer) which
    // races the current event dispatch and could miss the first click.
    core::AppManager::getInstance().queueSwitchApp("Files",
        core::System::getInstance().getAppPanel());
}

static lv_obj_t *create_files_item(lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    style_menu_item_icon(cont, LV_SYMBOL_DIRECTORY, "Files");
    lv_obj_add_event_cb(cont, files_click_cb, LV_EVENT_CLICKED, NULL);
    return cont;
}

static void power_off_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hw_feedback();
    lv_delay_ms(200);
    hw_shutdown();
}

static lv_obj_t *create_power_off_item(lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    style_menu_item_icon(cont, LV_SYMBOL_POWER, "Power Off");
    lv_obj_t *icon_label = lv_obj_get_child(cont, 0);
    if (icon_label) {
        lv_obj_set_style_text_color(icon_label,
                                    lv_palette_main(LV_PALETTE_RED), 0);
    }
    lv_obj_add_event_cb(cont, power_off_click_cb, LV_EVENT_CLICKED, NULL);
    return cont;
}


typedef void (*subpage_builder_t)(lv_obj_t *menu, lv_obj_t *page);

static void settings_page_changed_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    // Ignore events bubbling from children (like dropdowns)
    if (lv_event_get_target(e) != obj) return;

    lv_obj_t *page = lv_menu_get_cur_main_page(obj);
    if (page != settings_main_page) {
        subpage_builder_t builder = (subpage_builder_t)lv_obj_get_user_data(page);
        if (builder) {
            builder(menu, page);
            lv_obj_set_user_data(page, NULL); // Only build once
        }
        activate_subpage_group(page);
    } else {
        restore_main_page_group();
    }
}

static void settings_exit_cb(lv_event_t *e)
{
    // On the root settings page the status bar back exits the app entirely.
    // On a subpage it pops back to the root page. Forwarding a click to the
    // built-in back button proved unreliable (the header back is hidden/
    // zero-sized, and the event doesn't always drive lv_menu's pop), so
    // clear history and re-load the root page directly.
    if (menu) {
        lv_obj_t *page = lv_menu_get_cur_main_page(menu);
        if (page && page != settings_main_page) {
            lv_menu_clear_history(menu);
            lv_menu_set_page(menu, settings_main_page);
            return;
        }
    }
    // menu_show will trigger AppManager::switchApp which calls ui_sys_exit
    menu_show();
}

static lv_obj_t *create_subpage_fonts(lv_obj_t *menu, lv_obj_t *parent)
{
    lv_obj_t *cont = lv_menu_cont_create(parent);
    style_menu_item_icon(cont, LV_SYMBOL_EDIT, "Fonts");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&fonts_cfg::build_subpage);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

void toggle_child_focus_cb(lv_event_t * e) {
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

lv_obj_t *create_toggle_btn_row(lv_obj_t *parent, const char *txt, bool initial_state, lv_event_cb_t cb)
{
    lv_obj_t *obj = create_text(parent, NULL, txt, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *btn = lv_btn_create(obj);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE); 
    if (initial_state) lv_obj_add_state(btn, LV_STATE_CHECKED);
    
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_STATE_CHECKED);
    
    // Fix width to align "On/Off" texts to the right, like the standard labels
    lv_obj_set_width(btn, 60);
    lv_obj_set_flex_grow(btn, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, initial_state ? " On " : " Off ");
    lv_obj_center(label);
    
    lv_obj_set_user_data(btn, label);
    
    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_VALUE_CHANGED, NULL);

    return btn;
}

static lv_obj_t *create_subpage_storage(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_SD_CARD, "Storage");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&storage_cfg::build_subpage);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

static lv_obj_t *create_subpage_notes_security(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_EYE_CLOSE, "Notes Security");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&notes_sec_cfg::build_subpage);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    notes_sec_cfg::set_sub_page(sub_page);
    return cont;
}



static lv_obj_t *create_subpage_weather(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_GPS, "Weather");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&weather_cfg::build_subpage);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    weather_cfg::set_sub_page(sub_page);
    return cont;
}

static lv_obj_t *create_subpage_telegram(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_ENVELOPE, "Telegram");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&telegram_cfg::build_subpage);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    telegram_cfg::set_sub_page(sub_page);
    return cont;
}

static lv_obj_t *create_subpage_notes_sync(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_REFRESH, "Notes Sync");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&notes_sync_cfg::build_subpage);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    notes_sync_cfg::set_sub_page(sub_page);
    return cont;
}

static lv_obj_t *create_subpage_connectivity(lv_obj_t *menu, lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, LV_SYMBOL_WIFI, "Connectivity");
    lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(sub_page, (void*)&connectivity_cfg::build_subpage);
    lv_menu_set_load_page_event(menu, cont, sub_page);
    return cont;
}

void ui_sys_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    menu_g = lv_group_get_default();

    enable_keyboard();

    hw_get_user_setting(local_param);

    menu = lv_menu_create(parent);
#if LVGL_VERSION_MAJOR == 9
    lv_menu_set_mode_root_back_button(menu, LV_MENU_ROOT_BACK_BUTTON_DISABLED);
#else
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_DISABLED);
#endif
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(100));
    lv_obj_center(menu);

    // Suppress the menu's built-in header back button: LVGL re-un-hides it on
    // each subpage, so a plain HIDDEN flag on the header doesn't stick. Zero
    // its size and styling so the header's content_height stays 0 and LVGL's
    // own refr_main_header_mode hides the whole header automatically. It
    // remains programmatically clickable for the status bar back to drive.
    lv_obj_t *bb = lv_menu_get_main_header_back_button(menu);
    if (bb) {
        lv_obj_set_size(bb, 0, 0);
        lv_obj_set_style_pad_all(bb, 0, 0);
        lv_obj_set_style_border_width(bb, 0, 0);
        lv_obj_set_style_outline_width(bb, 0, 0);
        lv_obj_set_style_shadow_width(bb, 0, 0);
        lv_obj_set_style_bg_opa(bb, LV_OPA_TRANSP, 0);
    }

    /*Create a main page*/
    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);

    lv_obj_t *cont;
    main_page_group_count = 0;
    subpage_item_count = 0;

    // Flatten the item list into a two-column grid: each lv_menu_cont gets
    // LV_PCT(48) width and the page wraps rows of two.
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(main_page, 6, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    auto add_grid_item = [&](lv_obj_t *c) {
        if (!c) return;
        lv_obj_set_width(c, LV_PCT(48));
        add_main_page_group_item(c);
    };

    // Back button lives on the status bar. Keep the pointer so
    // restore_main_page_group() can focus it when returning from a subpage.
    settings_exit_btn = ui_show_back_button(settings_exit_cb);

    cont = create_subpage_backlight(menu, main_page);    add_grid_item(cont);
    cont = create_subpage_fonts(menu, main_page);        add_grid_item(cont);
    cont = create_subpage_datetime(menu, main_page);     add_grid_item(cont);
    cont = create_subpage_otg(menu, main_page);          add_grid_item(cont);
    cont = create_subpage_connectivity(menu, main_page); add_grid_item(cont);
    cont = create_subpage_weather(menu, main_page);      add_grid_item(cont);
    cont = create_subpage_telegram(menu, main_page);     add_grid_item(cont);
    cont = create_subpage_notes_sync(menu, main_page);   add_grid_item(cont);
    cont = create_subpage_storage(menu, main_page);      add_grid_item(cont);
    cont = create_files_item(main_page);                 add_grid_item(cont);
    cont = create_subpage_notes_security(menu, main_page); add_grid_item(cont);
    cont = create_subpage_performance(menu, main_page);  add_grid_item(cont);
    cont = create_subpage_info(menu, main_page);         add_grid_item(cont);
    cont = create_device_probe(menu, main_page);         add_grid_item(cont);
    cont = create_power_off_item(main_page);             add_grid_item(cont);

    settings_main_page = main_page;
    lv_menu_set_page(menu, main_page);
    lv_obj_add_event_cb(menu, settings_page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Without this the encoder has no focus anchor on the very first entry
    // (the menu tiles that used to hold focus were just cleaned up by
    // switchApp), so rotation does nothing until something is touched.
    if (main_page_group_count > 0) {
        lv_group_focus_obj(main_page_group_items[0]);
    } else if (settings_exit_btn) {
        lv_group_focus_obj(settings_exit_btn);
    }

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        settings_exit_cb(e);
    }, NULL);
#endif
}


void ui_sys_exit(lv_obj_t *parent)
{
    info_cfg::reset_state();
    ui_hide_back_button();
    // Intentionally do NOT disable_keyboard() here: the next app (e.g. Files
    // browser) immediately calls enable_keyboard(), which on T-LoRa-Pager
    // cycles the TCA8418 (kb.end()/kb.begin() → detach/reattach ISR + ledc).
    // That cycle races with the core-0 keyboard_task polling the same chip
    // and hangs the device. Keyboard is a system-level resource; leave it on
    // across app switches. Sleep path (ui_pause_timers) still disables it.
    if (menu) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
    }
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    hw_set_user_setting(local_param);
    // Rebuild the theme so any system-font change takes effect on the next
    // screen that gets constructed. Objects already created keep their old
    // font, but the next UI (menu, app, dialog) picks up the new one.
    theme_init();
    settings_main_page = NULL;
    settings_exit_btn = NULL;
    // Drop any in-flight NTP poll timer so it doesn't outlive the page it
    // was driving and touch a deleted status label.
    datetime_cfg::reset_state();
    connectivity_cfg::reset_state();
    notes_sec_cfg::reset_state();
    weather_cfg::reset_state();
    telegram_cfg::reset_state();
    notes_sync_cfg::reset_state();
    main_page_group_count = 0;
    subpage_item_count = 0;
    memset(main_page_group_items, 0, sizeof(main_page_group_items));
    memset(subpage_items, 0, sizeof(subpage_items));
}

#include "app_registry.h"

namespace {
class SysApp : public core::App {
public:
    SysApp() : core::App("Settings") {}
    void onStart(lv_obj_t *parent) override { ui_sys_enter(parent); }
    void onStop() override {
        ui_sys_exit(getRoot());
        core::App::onStop();
    }
};
} // namespace

namespace apps {
APP_FACTORY(make_sys_app, SysApp)
} // namespace apps
