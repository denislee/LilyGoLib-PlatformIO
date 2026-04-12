/**
 * @file      ui_text_editor.cpp
 * @author    LilyGo CLI Agent
 * @license   MIT
 * @date      2026-04-10
 *
 */
#include "ui_define.h"

static lv_obj_t *menu = NULL;
static lv_obj_t *text_area = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *exit_cont = NULL;
static lv_obj_t *word_count_label = NULL;
static string current_file_path = "";

static void update_word_count()
{
    if (!text_area || !word_count_label) return;
    
    const char *txt = lv_textarea_get_text(text_area);
    size_t chars = lv_strlen(txt);
    size_t words = 0;
    bool in_word = false;
    
    for (size_t i = 0; i < chars; i++) {
        if (isspace((unsigned char)txt[i])) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    
    lv_label_set_text_fmt(word_count_label, "%zu words | %zu chars", words, chars);
}

static void save_content()
{
    if (!text_area) return;
    const char *txt = lv_textarea_get_text(text_area);
    
    // Check if content is empty or only whitespace
    bool only_whitespace = true;
    size_t len = lv_strlen(txt);
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)txt[i])) {
            only_whitespace = false;
            break;
        }
    }

    if (only_whitespace) {
        printf("Content is empty or only whitespace, skip save\n");
        return;
    }

    bool success = false;
    string target_path = current_file_path;

    if (target_path.empty()) {
        struct tm timeinfo;
        hw_get_date_time(timeinfo);
        char filename[64];
        snprintf(filename, sizeof(filename), "/%04d%02d%02d_%02d%02d%02d.txt",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        target_path = filename;
    }

    printf("Saving to %s...\n", target_path.c_str());
    success = hw_save_file(target_path.c_str(), txt);
    
    if (success) {
        printf("Save successful\n");
    } else {
        printf("Save failed!\n");
        ui_msg_pop_up("Save", "Failed to save file!");
    }
}

static void do_exit()
{
    save_content();
    lv_obj_clean(menu);
    lv_obj_del(menu);
    menu = NULL;
    text_area = NULL;
    exit_cont = NULL;
    word_count_label = NULL;
    disable_keyboard();
    menu_show();
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    current_file_path = "";
}

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        do_exit();
    }
}

static void exit_btn_cb(lv_event_t *e)
{
    do_exit();
}

static void text_area_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(obj);

    if (code == LV_EVENT_VALUE_CHANGED) {
        update_word_count();
    } else if (code == LV_EVENT_CLICKED) {
        bool editing = lv_group_get_editing(g);
        lv_group_set_editing(g, !editing);
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == 0x1E) {
            lv_event_stop_processing(e);
            return;
        }
        if (key == LV_KEY_ESC) {
            bool editing = lv_group_get_editing(g);
            if (editing) {
                lv_group_set_editing(g, false);
            } else {
                /* ESC when not editing -> exit the editor */
                do_exit();
            }
            lv_event_stop_processing(e);
        }
    }
}

void ui_text_editor_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    enable_keyboard();

    menu = create_menu(parent, back_event_handler);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);

    /* Header for Exit button and Word Count */
    exit_cont = lv_menu_cont_create(main_page);
    lv_obj_set_flex_flow(exit_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(exit_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *exit_btn = lv_btn_create(exit_cont);
    lv_obj_t *exit_label = lv_label_create(exit_btn);
    lv_label_set_text(exit_label, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(exit_btn, exit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(lv_group_get_default(), exit_btn);
    
    word_count_label = lv_label_create(exit_cont);
    lv_label_set_text(word_count_label, "0 words | 0 chars");
    lv_obj_set_style_text_color(word_count_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_text_font(word_count_label, get_small_font(), 0);

    /* Textarea fills remaining space */
    text_area = lv_textarea_create(main_page);
    lv_obj_set_width(text_area, LV_PCT(100));
    lv_obj_set_flex_grow(text_area, 1);
    lv_textarea_set_placeholder_text(text_area, "");
    lv_obj_set_style_text_font(text_area, get_editor_font(), 0);
    lv_obj_set_style_border_width(text_area, 0, 0);

    /* Cursor style */
    lv_obj_set_style_bg_color(text_area, lv_color_white(), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(text_area, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_width(text_area, 8, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(text_area, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(text_area, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    lv_obj_add_event_cb(text_area, text_area_event_cb, LV_EVENT_ALL, NULL);
    lv_group_add_obj(lv_group_get_default(), text_area);

    lv_menu_set_page(menu, main_page);

    lv_group_focus_obj(text_area);
    update_word_count();

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        do_exit();
    }, NULL);
#endif
}

void ui_text_editor_open_file(const char *path)
{
    if (text_area == NULL) return;
    current_file_path = path;
    string content;
    if (hw_read_file(path, content)) {
        lv_textarea_set_text(text_area, content.c_str());
        update_word_count();
    }
}

void ui_text_editor_exit(lv_obj_t *parent)
{
    disable_keyboard();
}

app_t ui_text_editor_main = {
    .setup_func_cb = ui_text_editor_enter,
    .exit_func_cb = ui_text_editor_exit,
    .user_data = nullptr,
};
