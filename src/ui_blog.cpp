/**
 * @file      ui_blog.cpp
 * @author    LilyGo CLI Agent
 * @license   MIT
 * @date      2026-04-10
 *
 */
#include "ui_define.h"
#include <vector>
#include <string>
#include <algorithm>

static lv_obj_t *menu = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *parent_obj = NULL;
static int target_focus_index = -1;

static void do_exit()
{
    lv_obj_clean(menu);
    lv_obj_del(menu);
    menu = NULL;
    menu_show();
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    target_focus_index = -1;
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

static void post_focus_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_scroll_to_view(obj, LV_ANIM_ON);
}

static void post_click_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(obj);
    if (g) {
        // Prevent getting "stuck" in editing mode if the user clicks the scrollwheel
        lv_group_set_editing(g, false);
    }
    lv_event_stop_processing(e);
}

static void delete_msgbox_cb(lv_event_t *e)
{
    uint16_t id = 0;
    lv_obj_t *mbox_to_del = NULL;
#if LVGL_VERSION_MAJOR == 9
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *text = lv_label_get_text(label);
    if (strcmp(text, "No") == 0) {
        id = 1;
    }
    mbox_to_del = lv_obj_get_parent(lv_obj_get_parent(btn));
#else
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    id = lv_msgbox_get_active_btn(obj);
    mbox_to_del = obj;
#endif

    const char *path = (const char *)lv_event_get_user_data(e);
    bool deleted = false;

    if (id == 0) { // Yes
        if (hw_delete_file(path)) {
            printf("Deleted file: %s\n", path);
            deleted = true;
        }
    } else {
        target_focus_index = -1;
    }

    // IMPORTANT: Destroy msgbox FIRST to restore the correct group
    destroy_msgbox(mbox_to_del);

    if (deleted && ui_blog_main.setup_func_cb) {
        // Now refresh the UI
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
        if (quit_btn) {
            lv_obj_del_async(quit_btn);
            quit_btn = NULL;
        }
        (*ui_blog_main.setup_func_cb)(parent_obj);
    }

    if (path) lv_mem_free((void *)path);
}

static void show_delete_confirm(const char *filename)
{
    static const char *btns[] = {"Yes", "No", ""};
    char msg[128];
    snprintf(msg, sizeof(msg), "Delete this post?\n%s", filename);
    
    // Duplicate filename to pass to callback
    char *path_dup = (char *)lv_mem_alloc(strlen(filename) + 1);
    strcpy(path_dup, filename);

    create_msgbox(NULL, "Confirm", msg, btns, delete_msgbox_cb, path_dup);
}

static void blog_key_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == 'd' || key == 'D') {
            lv_group_t *g = lv_group_get_default();
            lv_obj_t *focused = lv_group_get_focused(g);
            if (focused) {
                const char *filename = (const char *)lv_obj_get_user_data(focused);
                if (filename) {
                    // Find current index among children of the main page
                    // We need to find which child index this 'focused' object is.
                    lv_obj_t *page = lv_menu_get_cur_main_page(menu);
                    if (page) {
                        uint32_t i;
                        for(i = 0; i < lv_obj_get_child_count(page); i++) {
                            if (lv_obj_get_child(page, i) == focused) {
                                target_focus_index = (int)i;
                                break;
                            }
                        }
                    }
                    show_delete_confirm(filename);
                }
            }
        }
    }
}

static std::string parse_filename_to_human(const std::string &filename)
{
    // Format: /YYYYMMDD_HHMMSS.txt
    size_t start = (filename[0] == '/') ? 1 : 0;
    if (filename.length() < start + 15) return filename;

    std::string y = filename.substr(start, 4);
    std::string m = filename.substr(start + 4, 2);
    std::string d = filename.substr(start + 6, 2);
    std::string hh = filename.substr(start + 9, 2);
    std::string mm = filename.substr(start + 11, 2);
    std::string ss = filename.substr(start + 13, 2);

    const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int month_idx = atoi(m.c_str()) - 1;
    if (month_idx < 0 || month_idx > 11) return filename;

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s %s, %s - %s:%s:%s", 
             months[month_idx], d.c_str(), y.c_str(), hh.c_str(), mm.c_str(), ss.c_str());
    return std::string(buffer);
}

void ui_blog_enter(lv_obj_t *parent)
{
    enable_keyboard();
    parent_obj = parent;
    menu = create_menu(parent, back_event_handler);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_all(main_page, 10, 0);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(main_page, LV_OBJ_FLAG_SCROLLABLE);

    // Capture keys on the main page
    lv_obj_add_event_cb(main_page, blog_key_event_cb, LV_EVENT_KEY, NULL);

    /* Exit button */
    lv_obj_t *exit_cont = lv_menu_cont_create(main_page);
    lv_obj_t *exit_btn = lv_btn_create(exit_cont);
    lv_obj_t *exit_label = lv_label_create(exit_btn);
    lv_label_set_text(exit_label, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(exit_btn, exit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(lv_group_get_default(), exit_btn);
    lv_obj_add_event_cb(exit_btn, post_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(exit_btn, blog_key_event_cb, LV_EVENT_KEY, NULL);

    std::vector<std::string> txt_files;
    hw_get_txt_files(txt_files);

    lv_obj_t *focus_target = exit_btn;
    int current_child_idx = 0; // index 0 is exit_cont

    if (txt_files.empty()) {
        lv_obj_t *empty_label = lv_label_create(main_page);
        lv_label_set_text(empty_label, "No blog posts yet.");
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty_label, lv_pct(100));
    } else {
        const lv_font_t *font = get_small_font();

        for (const auto &filename : txt_files) {
            current_child_idx++;
            // Container for each post
            lv_obj_t *post_cont = lv_obj_create(main_page);
            lv_obj_set_width(post_cont, lv_pct(100));
            lv_obj_set_height(post_cont, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(post_cont, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(post_cont, 5, 0);
            lv_obj_set_style_border_width(post_cont, 0, 0);
            lv_obj_set_style_bg_opa(post_cont, LV_OPA_TRANSP, 0);
            
            // Store filename for deletion
            char *fn_dup = (char *)lv_mem_alloc(filename.length() + 1);
            strcpy(fn_dup, filename.c_str());
            lv_obj_set_user_data(post_cont, fn_dup);

            // Standard focus style for visual feedback
            lv_obj_set_style_border_width(post_cont, 2, LV_STATE_FOCUSED);
            lv_obj_set_style_border_color(post_cont, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_FOCUSED);
            lv_obj_set_style_radius(post_cont, 5, 0);

            // Add to group for encoder scrolling
            lv_group_add_obj(lv_group_get_default(), post_cont);
            lv_obj_add_event_cb(post_cont, post_focus_cb, LV_EVENT_FOCUSED, NULL);
            lv_obj_add_event_cb(post_cont, post_click_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(post_cont, blog_key_event_cb, LV_EVENT_KEY, NULL);

            if (target_focus_index != -1) {
                if (current_child_idx == target_focus_index) {
                    focus_target = post_cont;
                } else if (current_child_idx < target_focus_index) {
                    // Fallback to latest valid post if exact index is now out of bounds
                    focus_target = post_cont;
                }
            }

            // Header with date
            lv_obj_t *header = lv_label_create(post_cont);
            lv_label_set_text(header, parse_filename_to_human(filename).c_str());
            lv_obj_set_style_text_font(header, font, 0);
            lv_obj_set_style_text_color(header, lv_palette_main(LV_PALETTE_GREY), 0);
            lv_obj_set_width(header, lv_pct(100));

            // Horizontal line
            lv_obj_t *line = lv_obj_create(post_cont);
            lv_obj_set_size(line, lv_pct(100), 1);
            lv_obj_set_style_bg_color(line, lv_palette_main(LV_PALETTE_GREY), 0);
            lv_obj_set_style_border_width(line, 0, 0);

            // Content
            std::string content;
            if (hw_read_file(filename.c_str(), content)) {
                lv_obj_t *label = lv_label_create(post_cont);
                lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
                lv_label_set_text(label, content.c_str());
                lv_obj_set_width(label, lv_pct(100));
                lv_obj_set_style_text_font(label, font, 0);
                lv_obj_set_style_text_color(label, lv_color_white(), 0);
            }
        }
    }

    lv_menu_set_page(menu, main_page);

    if (focus_target) {
        lv_group_focus_obj(focus_target);
        lv_obj_scroll_to_view(focus_target, LV_ANIM_OFF);
    }

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        do_exit();
    }, NULL);
#endif
}

void ui_blog_exit(lv_obj_t *parent)
{
    disable_keyboard();
    // Clean up allocated strings in user_data when exiting
    if (menu) {
        lv_obj_t *main_page = lv_menu_get_cur_main_page(menu);
        if (main_page) {
            uint32_t i;
            for(i = 0; i < lv_obj_get_child_count(main_page); i++) {
                lv_obj_t *child = lv_obj_get_child(main_page, i);
                void *data = lv_obj_get_user_data(child);
                if (data) lv_mem_free(data);
            }
        }
    }
    target_focus_index = -1;
}

app_t ui_blog_main = {
    .setup_func_cb = ui_blog_enter,
    .exit_func_cb = ui_blog_exit,
    .user_data = nullptr,
};
