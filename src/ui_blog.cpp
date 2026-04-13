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
static int current_page = 0;
#define BLOG_PAGE_SIZE 5
static std::vector<std::string> blog_files_cache;
static bool cache_valid = false;

void ui_blog_enter(lv_obj_t *parent);
void ui_blog_exit(lv_obj_t *parent);

static void do_exit()
{
    ui_blog_exit(NULL);
    lv_obj_clean(menu);
    lv_obj_del(menu);
    menu = NULL;
    menu_show();
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    target_focus_index = -1;
    current_page = 0;
    cache_valid = false;
    blog_files_cache.clear();
}

static void next_page_cb(lv_event_t *e)
{
    current_page++;
    lv_obj_clean(menu);
    lv_obj_del(menu);
    menu = NULL;
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    ui_blog_enter(parent_obj);
}

static void prev_page_cb(lv_event_t *e)
{
    if (current_page > 0) {
        current_page--;
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
        if (quit_btn) {
            lv_obj_del_async(quit_btn);
            quit_btn = NULL;
        }
        ui_blog_enter(parent_obj);
    }
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
        bool editing = lv_group_get_editing(g);
        lv_group_set_editing(g, !editing);
    }
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
            cache_valid = false;
        }
    } else {
        target_focus_index = -1;
    }

    // IMPORTANT: Destroy msgbox FIRST to restore the correct group
    destroy_msgbox(mbox_to_del);

    if (deleted && ui_blog_main.setup_func_cb) {
        // Now refresh the UI
        ui_blog_exit(NULL);
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
        lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
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
        } else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN) {
            lv_group_t *g = lv_obj_get_group(obj);
            if (g && lv_group_get_editing(g)) {
                lv_coord_t y = lv_obj_get_scroll_y(obj);
                if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) {
                    y += 40;
                } else if (key == LV_KEY_LEFT || key == LV_KEY_UP) {
                    y -= 40;
                }
                lv_obj_scroll_to_y(obj, y, LV_ANIM_ON);
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

static void post_scroll_indicator_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *indicator = (lv_obj_t *)lv_event_get_user_data(e);
    if (!indicator) return;
    
    if (lv_obj_get_scroll_bottom(obj) <= 0) {
        lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_blog_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
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

    if (!cache_valid) {
        hw_get_txt_files(blog_files_cache);
        // Ensure newest (latest YYYYMMDD_HHMMSS) are on top
        std::sort(blog_files_cache.begin(), blog_files_cache.end(), std::greater<std::string>());
        cache_valid = true;
    }

    int total_files = blog_files_cache.size();
    int start_idx = current_page * BLOG_PAGE_SIZE;
    int end_idx = std::min(start_idx + BLOG_PAGE_SIZE, total_files);

    if (start_idx >= total_files && total_files > 0) {
        current_page = (total_files - 1) / BLOG_PAGE_SIZE;
        start_idx = current_page * BLOG_PAGE_SIZE;
        end_idx = total_files;
    }

    lv_obj_t *focus_target = exit_btn;
    int current_child_idx = 0; // index 0 is exit_cont

    if (total_files == 0) {
        lv_obj_t *empty_label = lv_label_create(main_page);
        lv_label_set_text(empty_label, "No blog posts yet.");
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty_label, lv_pct(100));
    } else {
        const lv_font_t *font = get_small_font();

        // Prev Page Button
        if (current_page > 0) {
            lv_obj_t *prev_cont = lv_menu_cont_create(main_page);
            lv_obj_t *prev_btn = lv_btn_create(prev_cont);
            lv_obj_t *prev_label = lv_label_create(prev_btn);
            lv_label_set_text(prev_label, LV_SYMBOL_UP " Previous Page");
            lv_obj_add_event_cb(prev_btn, prev_page_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), prev_btn);
            lv_obj_add_event_cb(prev_btn, post_focus_cb, LV_EVENT_FOCUSED, NULL);
            current_child_idx++;
        }

        for (int i = start_idx; i < end_idx; ++i) {
            const auto &filename = blog_files_cache[i];
            current_child_idx++;
            // Container for each post
            lv_obj_t *post_cont = lv_obj_create(main_page);
            lv_obj_set_width(post_cont, lv_pct(100));
            lv_obj_set_height(post_cont, LV_SIZE_CONTENT);
            lv_obj_set_style_max_height(post_cont, 150, 0);
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
            lv_obj_set_style_border_color(post_cont, lv_palette_main(LV_PALETTE_ORANGE), LV_STATE_FOCUSED | LV_STATE_EDITED);
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
                
                lv_obj_t *indicator = lv_label_create(post_cont);
                lv_label_set_text(indicator, LV_SYMBOL_DOWN);
                lv_obj_add_flag(indicator, LV_OBJ_FLAG_FLOATING);
                lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(indicator, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
                lv_obj_set_style_text_color(indicator, lv_palette_main(LV_PALETTE_ORANGE), 0);
                lv_obj_add_event_cb(post_cont, post_scroll_indicator_cb, LV_EVENT_SCROLL, indicator);
            }
        }

        // Footer container
        lv_obj_t *footer_cont = lv_obj_create(main_page);
        lv_obj_set_width(footer_cont, lv_pct(100));
        lv_obj_set_height(footer_cont, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(footer_cont, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(footer_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(footer_cont, 5, 0);
        lv_obj_set_style_bg_opa(footer_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(footer_cont, 0, 0);

        // Next Page Button
        if (end_idx < total_files) {
            lv_obj_t *next_btn = lv_btn_create(footer_cont);
            lv_obj_t *next_label = lv_label_create(next_btn);
            lv_label_set_text(next_label, LV_SYMBOL_DOWN " Next");
            lv_obj_add_event_cb(next_btn, next_page_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), next_btn);
            lv_obj_add_event_cb(next_btn, post_focus_cb, LV_EVENT_FOCUSED, NULL);
        }

        // Page indicator
        lv_obj_t *page_info = lv_label_create(footer_cont);
        char buf[32];
        snprintf(buf, sizeof(buf), "Page %d/%d", current_page + 1, (total_files + BLOG_PAGE_SIZE - 1) / BLOG_PAGE_SIZE);
        lv_label_set_text(page_info, buf);
        lv_obj_set_style_text_font(page_info, font, 0);
        lv_obj_set_style_text_color(page_info, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_flex_grow(page_info, 1);
        lv_obj_set_style_text_align(page_info, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_pad_right(page_info, 10, 0);
    }

    lv_menu_set_page(menu, main_page);

    // Force layout computation so scroll limits are accurate
    lv_obj_update_layout(menu);
    for (uint32_t i = 0; i < lv_obj_get_child_count(main_page); i++) {
        lv_obj_send_event(lv_obj_get_child(main_page, i), LV_EVENT_SCROLL, NULL);
    }

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
