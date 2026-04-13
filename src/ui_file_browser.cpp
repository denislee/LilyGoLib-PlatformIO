/**
 * @file      ui_file_browser.cpp
 * @author    LilyGo CLI Agent
 * @license   MIT
 * @date      2026-04-10
 *
 */
#include "ui_define.h"

static lv_obj_t *parent_obj = NULL;
static lv_obj_t *menu = NULL;
static lv_obj_t *file_list = NULL;
static lv_obj_t *quit_btn = NULL;

extern app_t ui_text_editor_main;

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        disable_keyboard();
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
        file_list = NULL;
        menu_show();
        if (quit_btn) {
            lv_obj_del_async(quit_btn);
            quit_btn = NULL;
        }
    }
}

static void file_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *filename = lv_list_get_button_text(file_list, btn);
    if (!filename) return;
    string path = "/" + string(filename);

    if (quit_btn) {
        lv_obj_del(quit_btn);
        quit_btn = NULL;
    }
    lv_obj_clean(menu);
    lv_obj_del(menu);
    menu = NULL;
    file_list = NULL;

    if (ui_text_editor_main.setup_func_cb) {
        (*ui_text_editor_main.setup_func_cb)(parent_obj);
        ui_text_editor_open_file(path.c_str());
    }
}

void ui_file_browser_refresh()
{
    if (file_list == NULL) return;
    lv_obj_clean(file_list);

    vector<string> txt_files;
    hw_get_txt_files(txt_files);

    int count = 0;
    for (const auto &file : txt_files) {
        if (file == "tasks.txt") continue;

        lv_obj_t *btn = lv_list_add_btn(file_list, LV_SYMBOL_FILE, file.c_str());
        lv_obj_add_event_cb(btn, file_click_cb, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(lv_group_get_default(), btn);
        count++;
    }

    if (count == 0) {
        lv_list_add_text(file_list, "No .txt files found");
    }
}

void ui_file_browser_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    menu = create_menu(parent, back_event_handler);
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_ENABLED);

    /* Add the menu back button to the group so the scroll wheel can reach it */
    lv_obj_t *back_btn = lv_menu_get_main_header_back_button(menu);
    if (back_btn) {
        lv_group_add_obj(lv_group_get_default(), back_btn);
    }

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);

    file_list = lv_list_create(main_page);
    lv_obj_set_size(file_list, LV_PCT(100), LV_PCT(100));

    ui_file_browser_refresh();

    lv_menu_set_page(menu, main_page);

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        lv_obj_t *page = lv_menu_get_cur_main_page(menu);
        if (lv_menu_back_button_is_root(menu, page)) {
             lv_obj_send_event(lv_menu_get_main_header_back_button(menu), LV_EVENT_CLICKED, NULL);
        } else {
             lv_obj_t *bb = lv_menu_get_main_header_back_button(menu);
             if (bb) lv_obj_send_event(bb, LV_EVENT_CLICKED, NULL);
        }
    }, NULL);
#endif
}

void ui_file_browser_exit(lv_obj_t *parent)
{
    disable_keyboard();
}

app_t ui_file_browser_main = {
    .setup_func_cb = ui_file_browser_enter,
    .exit_func_cb = ui_file_browser_exit,
    .user_data = nullptr,
};
