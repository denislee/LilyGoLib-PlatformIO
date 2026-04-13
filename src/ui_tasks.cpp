/**
 * @file      ui_tasks.cpp
 * @author    LilyGo CLI Agent
 * @license   MIT
 * @date      2026-04-10
 *
 */
#include "ui_define.h"
#include <vector>
#include <string>
#include <algorithm>

using namespace std;

struct TaskItem {
    bool is_task;
    bool checked;
    string text;
    lv_obj_t *obj;
};

static lv_obj_t *parent_obj = NULL;
static lv_obj_t *menu = NULL;
static lv_obj_t *main_page = NULL;
static lv_obj_t *add_ta = NULL;
static lv_obj_t *task_container = NULL;
static lv_obj_t *quit_btn = NULL;
static vector<TaskItem> tasks;

static const char *tasks_file_path = "/tasks.txt";

static void save_tasks() {
    string content = "";
    for (auto &t : tasks) {
        if (t.is_task) {
            content += (t.obj && lv_obj_has_state(t.obj, LV_STATE_CHECKED) ? "[x] " : "[ ] ");
            content += t.text + "\n";
        } else {
            content += t.text + "\n";
        }
    }
    hw_save_file(tasks_file_path, content.c_str());
}

void ui_tasks_refresh();

static void task_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        bool checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
        for (auto &t : tasks) {
            if (t.obj == obj) {
                string icon = checked ? LV_SYMBOL_OK : LV_SYMBOL_MINUS;
                string display_text = icon + "  " + t.text;
                lv_checkbox_set_text(obj, display_text.c_str());
                break;
            }
        }
        save_tasks();
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            // Find the task and remove it
            for (auto it = tasks.begin(); it != tasks.end(); ++it) {
                if (it->obj == obj) {
                    tasks.erase(it);
                    break;
                }
            }
            save_tasks();
            ui_tasks_refresh();
        }
    }
}

static void add_ta_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(ta);

    if (code == LV_EVENT_CLICKED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev && lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) {
            bool editing = lv_group_get_editing(g);
            lv_group_set_editing(g, !editing);
        }
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        bool editing = lv_group_get_editing(g);

        if (key == LV_KEY_ENTER) {
            if (!editing) {
                lv_group_set_editing(g, true);
                lv_event_stop_processing(e);
                return;
            }
            
            const char *txt = lv_textarea_get_text(ta);
            if (strlen(txt) > 0) {
                // Add new task
                TaskItem t;
                t.is_task = true;
                t.checked = false;
                t.text = txt;
                t.obj = NULL;
                tasks.insert(tasks.begin(), t); // Add at the top
                save_tasks();
                lv_textarea_set_text(ta, "");
                ui_tasks_refresh();
            }
            lv_group_set_editing(g, false);
            lv_event_stop_processing(e);
            return;
        }

        if (key == LV_KEY_ESC) {
            if (editing) {
                lv_group_set_editing(g, false);
                lv_event_stop_processing(e);
            } else {
                if (menu) {
                    lv_obj_t *bb = lv_menu_get_main_header_back_button(menu);
                    if (bb) lv_obj_send_event(bb, LV_EVENT_CLICKED, NULL);
                }
            }
        }
    }
}

static void back_event_handler(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        disable_keyboard();
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
        main_page = NULL;
        add_ta = NULL;
        task_container = NULL;
        tasks.clear();
        menu_show();
        if (quit_btn) {
            lv_obj_del_async(quit_btn);
            quit_btn = NULL;
        }
    }
}

void ui_tasks_refresh() {
    if (task_container == NULL) return;
    lv_obj_clean(task_container);
    tasks.clear();

    string content;
    if (!hw_read_file(tasks_file_path, content)) {
        content = ""; 
    }

    // Split content by \n
    size_t pos = 0;
    string token;
    string delim = "\n";
    while ((pos = content.find(delim)) != string::npos) {
        token = content.substr(0, pos);
        content.erase(0, pos + delim.length());
        
        if (!token.empty() && token.back() == '\r') {
            token.pop_back();
        }
        if (token.empty()) continue;

        TaskItem t;
        t.is_task = false;
        t.checked = false;

        if (token.rfind("[ ] ", 0) == 0) {
            t.is_task = true;
            t.text = token.substr(4);
        } else if (token.rfind("[x] ", 0) == 0 || token.rfind("[X] ", 0) == 0) {
            t.is_task = true;
            t.checked = true;
            t.text = token.substr(4);
        } else {
            t.text = token;
        }
        tasks.push_back(t);
    }
    // Handle last line if no trailing newline
    if (!content.empty()) {
        if (!content.empty() && content.back() == '\r') content.pop_back();
        if (!content.empty()) {
            TaskItem t;
            t.is_task = false;
            t.checked = false;
            if (content.rfind("[ ] ", 0) == 0) {
                t.is_task = true;
                t.text = content.substr(4);
            } else if (content.rfind("[x] ", 0) == 0 || content.rfind("[X] ", 0) == 0) {
                t.is_task = true;
                t.checked = true;
                t.text = content.substr(4);
            } else {
                t.text = content;
            }
            tasks.push_back(t);
        }
    }

    if (tasks.empty()) {
        return;
    }

    for (auto &t : tasks) {
        if (t.is_task) {
            t.obj = lv_checkbox_create(task_container);
            lv_obj_set_width(t.obj, LV_PCT(100));
            
            // Hide the default checkbox indicator box to make it a clean list
            lv_obj_set_style_opa(t.obj, LV_OPA_TRANSP, LV_PART_INDICATOR);
            lv_obj_set_style_width(t.obj, 0, LV_PART_INDICATOR);
            lv_obj_set_style_pad_all(t.obj, 0, LV_PART_INDICATOR);
            lv_obj_set_style_margin_all(t.obj, 0, LV_PART_INDICATOR);
            lv_obj_set_style_pad_left(t.obj, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_column(t.obj, 0, LV_PART_MAIN);

            string icon = t.checked ? LV_SYMBOL_OK : LV_SYMBOL_MINUS;
            string display_text = icon + "  " + t.text;
            lv_checkbox_set_text(t.obj, display_text.c_str());
            
            // Visual feedback when navigating to the task (focused state)
            lv_obj_set_style_border_color(t.obj, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_FOCUSED);
            lv_obj_set_style_border_width(t.obj, 2, LV_STATE_FOCUSED);
            lv_obj_set_style_bg_color(t.obj, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_FOCUSED);
            lv_obj_set_style_bg_opa(t.obj, LV_OPA_20, LV_STATE_FOCUSED);

            // Visual feedback when the task is done (checked state)
            lv_obj_set_style_text_decor(t.obj, LV_TEXT_DECOR_STRIKETHROUGH, LV_STATE_CHECKED);
            lv_obj_set_style_text_color(t.obj, lv_palette_main(LV_PALETTE_GREY), LV_STATE_CHECKED);

            if (t.checked) {
                lv_obj_add_state(t.obj, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(t.obj, task_event_cb, LV_EVENT_ALL, NULL);
            lv_group_add_obj(lv_group_get_default(), t.obj);
        } else {
            t.obj = lv_label_create(task_container);
            lv_obj_set_width(t.obj, LV_PCT(100));
            lv_obj_set_style_pad_left(t.obj, 0, LV_PART_MAIN);
            
            string display_text = string(LV_SYMBOL_BULLET) + " " + t.text;
            lv_label_set_text(t.obj, display_text.c_str());
            lv_obj_set_style_text_color(t.obj, lv_palette_main(LV_PALETTE_GREY), 0);
        }
    }
}

void ui_tasks_enter(lv_obj_t *parent) {
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    menu = create_menu(parent, back_event_handler);
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_ENABLED);

    lv_obj_t *back_btn = lv_menu_get_main_header_back_button(menu);
    if (back_btn) {
        lv_group_add_obj(lv_group_get_default(), back_btn);
    }

    main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(main_page, 0, 0); // Remove padding to flush left

    add_ta = lv_textarea_create(main_page);
    lv_textarea_set_one_line(add_ta, true);
    lv_textarea_set_placeholder_text(add_ta, "[new task]");
    lv_obj_set_width(add_ta, LV_PCT(100));

    lv_obj_set_style_border_width(add_ta, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(add_ta, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(add_ta, lv_palette_main(LV_PALETTE_ORANGE), LV_STATE_EDITED);
    
    // Set placeholder text color to grey
    lv_obj_set_style_text_color(add_ta, lv_palette_main(LV_PALETTE_GREY), LV_PART_TEXTAREA_PLACEHOLDER);

    lv_obj_add_event_cb(add_ta, add_ta_event_cb, LV_EVENT_ALL, NULL);
    lv_group_add_obj(lv_group_get_default(), add_ta);

    task_container = lv_obj_create(main_page);
    lv_obj_set_width(task_container, LV_PCT(100));
    lv_obj_set_flex_grow(task_container, 1);
    lv_obj_set_flex_flow(task_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(task_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(task_container, 0, 0);
    lv_obj_set_style_pad_all(task_container, 0, 0);

    ui_tasks_refresh();

    lv_menu_set_page(menu, main_page);
    lv_group_focus_obj(add_ta);

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

void ui_tasks_exit(lv_obj_t *parent) {
    disable_keyboard();
}

app_t ui_tasks_main = {
    .setup_func_cb = ui_tasks_enter,
    .exit_func_cb = ui_tasks_exit,
    .user_data = nullptr,
};

