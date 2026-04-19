/**
 * @file      ui_news.cpp
 * @brief     Plain-text news viewer from SD card /news directory.
 */
#include "ui_define.h"
#include "core/app_manager.h"
#include "hal/storage.h"
#include <vector>
#include <string>

namespace {

static lv_obj_t *menu = NULL;
static lv_obj_t *parent_obj = NULL;
static lv_obj_t *file_list = NULL;
static lv_obj_t *main_page = NULL;
static lv_obj_t *view_page = NULL;
static lv_obj_t *index_page = NULL;
static lv_obj_t *index_list = NULL;
static lv_obj_t *scroll_wrapper = NULL;
static lv_obj_t *text_area = NULL;

static lv_obj_t *lbl_progress = NULL;

static bool has_prev_page = false;
static bool has_next_page = false;

static std::vector<std::string> news_files;
static std::vector<std::pair<std::string, size_t>> news_headers;
static bool news_headers_loaded = false;

static std::string current_file_path = "";
static size_t current_file_size = 0;
static size_t current_file_start = 0;
static size_t current_file_end = 0;
static size_t current_file_offset = 0;
static size_t current_page_len = 0;
static const size_t NEWS_CHUNK_SIZE = 2048;
static std::vector<size_t> page_offsets;

void ui_news_exit(lv_obj_t *parent);
static void refresh_ui();
static void sync_menu_header();

static void load_files()
{
    news_files.clear();
    hw_get_sd_news_files(news_files);
}

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        ui_news_exit(NULL);
        menu_show();
    }
}

static void news_view_back_cb(lv_event_t *e)
{
    if (!menu) return;
    lv_obj_t *cur = lv_menu_get_cur_main_page(menu);
    if (cur == view_page) {
        lv_obj_t *target = (news_headers_loaded && !news_headers.empty()) ? index_page : main_page;
        lv_menu_set_page(menu, target);
        sync_menu_header();
    } else if (cur == index_page) {
        lv_menu_set_page(menu, main_page);
        sync_menu_header();
    } else {
        ui_news_exit(NULL);
        menu_show();
    }
}

static void sync_menu_header()
{
    if (!menu) return;
    lv_obj_t *header = lv_menu_get_main_header(menu);
    if (header) lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);
}

static void update_page()
{
    if (current_file_path.empty()) return;

    size_t remaining = (current_file_end > current_file_offset) ? (current_file_end - current_file_offset) : 0;
    size_t read_size = (remaining < NEWS_CHUNK_SIZE) ? remaining : NEWS_CHUNK_SIZE;
    bool is_final_chunk = (read_size == remaining);

    std::string content;
    if (read_size > 0 && !hw_read_file_chunk(current_file_path.c_str(), current_file_offset, read_size, content)) {
        ui_msg_pop_up("Error", "Failed to read file.");
        return;
    }
    if (content.length() > read_size) content.resize(read_size);

    // On the final chunk of this item, consume all the way to the item end
    // even if hw_read_file_chunk shortened the content at a word boundary —
    // the truncated tail is only the separator we want to drop anyway.
    current_page_len = is_final_chunk ? read_size : content.length();
    size_t next_raw_offset = current_file_offset + current_page_len;

    std::string display = content;
    if (is_final_chunk) {
        // Strip trailing separator line ("------") and surrounding whitespace
        // that precedes the next news item's header.
        while (!display.empty()) {
            char c = display.back();
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
                display.pop_back();
            } else {
                break;
            }
        }
        size_t dash_end = display.size();
        while (dash_end > 0 && display[dash_end - 1] == '-') dash_end--;
        if (dash_end < display.size() && (display.size() - dash_end) >= 3) {
            display.resize(dash_end);
            while (!display.empty()) {
                char c = display.back();
                if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
                    display.pop_back();
                } else {
                    break;
                }
            }
        }
    }

    lv_label_set_text(text_area, display.c_str());

    lv_obj_scroll_to_y(scroll_wrapper, 0, LV_ANIM_OFF);

    size_t next_offset = current_file_offset + current_page_len;
    size_t span = (current_file_end > current_file_start) ? (current_file_end - current_file_start) : 0;
    size_t consumed = (next_offset > current_file_start) ? (next_offset - current_file_start) : 0;

    int percent = 0;
    if (span > 0) {
        percent = (int)(((uint64_t)consumed * 100) / span);
    } else {
        percent = 100;
    }

    lv_label_set_text_fmt(lbl_progress, "%d%%", percent);

    has_prev_page = (current_file_offset > current_file_start);
    has_next_page = (next_offset < current_file_end);

    lv_group_focus_obj(scroll_wrapper);
    lv_group_set_editing(lv_group_get_default(), true);
}

static void prev_btn_cb(lv_event_t *e)
{
    if (page_offsets.size() > 1) {
        page_offsets.pop_back();
        current_file_offset = page_offsets.back();
        update_page();

        if (scroll_wrapper) {
            lv_obj_update_layout(scroll_wrapper);
            int32_t bottom = lv_obj_get_scroll_bottom(scroll_wrapper);
            int32_t current = lv_obj_get_scroll_y(scroll_wrapper);
            lv_obj_scroll_to_y(scroll_wrapper, current + bottom, LV_ANIM_OFF);
        }
    }
}

static void next_btn_cb(lv_event_t *e)
{
    if (current_page_len == 0) return;
    current_file_offset += current_page_len;
    page_offsets.push_back(current_file_offset);
    update_page();
}

static std::string resolve_news_path(const char *filename)
{
    std::string path = filename;
    if (path.empty()) return path;
    if (path[0] != '/') path = "/" + path;
    if (path.find("/news/") != 0) {
        if (path[0] == '/') path = "/news" + path;
        else path = "/news/" + path;
    }
    return path;
}

static void delete_msgbox_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *text = lv_label_get_text(label);
    bool confirmed = (strcmp(text, "Yes") == 0);
    lv_obj_t *mbox_to_del = lv_obj_get_parent(lv_obj_get_parent(btn));

    const char *path = (const char *)lv_event_get_user_data(e);

    if (confirmed && path) {
        if (hw_delete_file(path)) {
            load_files();
            refresh_ui();
        } else {
            ui_msg_pop_up("Error", "Failed to delete file.");
        }
    }

    destroy_msgbox(mbox_to_del);
    if (path) lv_mem_free((void *)path);
}

static void show_delete_confirm(const std::string &path)
{
    static const char *btns[] = {"Yes", "No", ""};
    char msg[160];
    snprintf(msg, sizeof(msg), "Delete this file?\n%s", path.c_str());

    char *path_dup = (char *)lv_mem_alloc(path.size() + 1);
    strcpy(path_dup, path.c_str());

    create_msgbox(NULL, "Confirm", msg, btns, delete_msgbox_cb, path_dup);
}

static void file_list_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_BACKSPACE) return;

    lv_group_t *g = lv_group_get_default();
    lv_obj_t *focused = lv_group_get_focused(g);
    if (!focused) return;

    const char *filename = lv_list_get_button_text(file_list, focused);
    if (!filename) return;

    show_delete_confirm(resolve_news_path(filename));
}

static void jump_to_range(size_t start, size_t end)
{
    current_file_start = start;
    current_file_end = end;
    current_file_offset = start;
    current_page_len = 0;
    page_offsets.clear();
    page_offsets.push_back(start);

    lv_menu_set_page(menu, view_page);
    sync_menu_header();
    update_page();
}

static void index_click_cb(lv_event_t *e)
{
    size_t idx = (size_t)lv_event_get_user_data(e);
    if (idx >= news_headers.size()) return;
    size_t start = news_headers[idx].second;
    size_t end = (idx + 1 < news_headers.size()) ? news_headers[idx + 1].second : current_file_size;
    jump_to_range(start, end);
}

static bool index_progress_cb(size_t current, size_t total)
{
    lv_timer_handler();
    return true;
}

static void show_index_page()
{
    lv_obj_clean(index_list);
    size_t total = news_headers.size();
    int idx_width = 1;
    for (size_t t = total; t >= 10; t /= 10) idx_width++;
    for (size_t i = 0; i < total; i++) {
        char title[256];
        snprintf(title, sizeof(title), "%*zu. %s", idx_width, i + 1, news_headers[i].first.c_str());
        lv_obj_t *btn = lv_list_add_btn(index_list, NULL, title);
        lv_obj_add_event_cb(btn, index_click_cb, LV_EVENT_CLICKED, (void *)i);

        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_obj_set_width(lbl, 0);
            lv_obj_set_flex_grow(lbl, 1);

            const lv_font_t *font = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
            if (font) {
                lv_obj_set_height(lbl, lv_font_get_line_height(font));
            }

            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        }

        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(ev);
            lv_obj_t *l = lv_obj_get_child(b, 0);
            if (l) lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
        }, LV_EVENT_FOCUSED, NULL);

        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(ev);
            lv_obj_t *l = lv_obj_get_child(b, 0);
            if (l) lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        }, LV_EVENT_DEFOCUSED, NULL);

        lv_group_add_obj(lv_group_get_default(), btn);
    }

    if (lv_obj_get_child_count(index_list) > 0) {
        lv_group_focus_obj(lv_obj_get_child(index_list, 0));
    }

    lv_menu_set_page(menu, index_page);
    sync_menu_header();
}

static void file_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *filename = lv_list_get_button_text(file_list, btn);
    if (!filename) return;

    std::string path = resolve_news_path(filename);

    current_file_path = path;
    current_file_size = hw_get_file_size(current_file_path.c_str());
    news_headers.clear();
    news_headers_loaded = false;
    current_file_offset = 0;
    current_page_len = 0;
    page_offsets.clear();
    page_offsets.push_back(0);

    lv_obj_t *loading_cont = lv_obj_create(lv_screen_active());
    lv_obj_set_size(loading_cont, 200, 120);
    lv_obj_center(loading_cont);
    lv_obj_set_style_bg_color(loading_cont, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(loading_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(loading_cont, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(loading_cont, UI_BORDER_W, 0);
    lv_obj_set_style_radius(loading_cont, UI_RADIUS, 0);
    lv_obj_set_flex_flow(loading_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(loading_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *loading_label = lv_label_create(loading_cont);
    lv_label_set_text(loading_label, "Loading indexes...");

    lv_refr_now(NULL);

    hw_get_news_headers(current_file_path.c_str(), news_headers, index_progress_cb);
    news_headers_loaded = true;
    lv_obj_del(loading_cont);

    if (news_headers.empty()) {
        // No index — open the file directly.
        jump_to_range(0, current_file_size);
    } else {
        show_index_page();
    }
}

static void refresh_ui()
{
    if (!file_list) return;
    lv_obj_clean(file_list);

    if (news_files.empty()) {
        lv_list_add_text(file_list, "No files found in /news");
    } else {
        for (const auto &file : news_files) {
            lv_obj_t *btn = lv_list_add_btn(file_list, LV_SYMBOL_FILE, file.c_str());
            lv_obj_add_event_cb(btn, file_click_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), btn);

            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            lv_obj_t *icon = lv_obj_get_child(btn, 0);
            lv_obj_t *lbl = lv_obj_get_child(btn, 1);
            if (icon) {
                lv_obj_set_width(icon, 20);
                lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
            }
            if (lbl) {
                lv_obj_set_width(lbl, 0);
                lv_obj_set_flex_grow(lbl, 1);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            }
        }
    }

    if (lv_obj_get_child_count(file_list) > 0) {
        lv_group_focus_obj(lv_obj_get_child(file_list, 0));
    }
}

void ui_news_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    menu = create_menu(parent, back_event_handler);
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_ENABLED);

    lv_obj_t *bb = lv_menu_get_main_header_back_button(menu);
    if (bb) {
        lv_obj_set_size(bb, 0, 0);
        lv_obj_set_style_pad_all(bb, 0, 0);
        lv_obj_set_style_border_width(bb, 0, 0);
        lv_obj_set_style_outline_width(bb, 0, 0);
        lv_obj_set_style_shadow_width(bb, 0, 0);
        lv_obj_set_style_bg_opa(bb, LV_OPA_TRANSP, 0);
    }

    ui_show_back_button(news_view_back_cb);

    main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);

    file_list = lv_list_create(main_page);
    lv_obj_set_width(file_list, LV_PCT(100));
    lv_obj_set_flex_grow(file_list, 1);
    lv_obj_add_event_cb(file_list, file_list_key_cb, LV_EVENT_KEY, NULL);

    view_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(view_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(view_page, 0, 0);

    scroll_wrapper = lv_obj_create(view_page);
    lv_obj_set_width(scroll_wrapper, LV_PCT(100));
    lv_obj_set_flex_grow(scroll_wrapper, 1);
    lv_obj_set_scroll_dir(scroll_wrapper, LV_DIR_VER);
    lv_obj_add_flag(scroll_wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scroll_wrapper, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_border_width(scroll_wrapper, UI_BORDER_W, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(scroll_wrapper, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(scroll_wrapper, 5, 0);

    lv_obj_add_event_cb(scroll_wrapper, [](lv_event_t *ev) {
        lv_obj_t *wrapper = (lv_obj_t *)lv_event_get_current_target(ev);
        lv_group_t *g = lv_group_get_default();
        if (!lv_group_get_editing(g) || lv_group_get_focused(g) != wrapper) return;

        uint32_t key = lv_event_get_key(ev);
        if (key == LV_KEY_UP || key == LV_KEY_LEFT) {
            int32_t cur_y = lv_obj_get_scroll_y(wrapper);
            if (cur_y == 0 && has_prev_page) {
                prev_btn_cb(NULL);
            } else {
                int32_t step = 40;
                if (cur_y < step) step = cur_y;
                if (step > 0) lv_obj_scroll_to_y(wrapper, cur_y - step, LV_ANIM_ON);
            }
            lv_event_stop_processing(ev);
        } else if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) {
            int32_t bottom = lv_obj_get_scroll_bottom(wrapper);
            int32_t cur_y = lv_obj_get_scroll_y(wrapper);
            if (bottom == 0 && has_next_page) {
                next_btn_cb(NULL);
            } else {
                int32_t step = 40;
                if (bottom < step) step = bottom;
                if (step > 0) lv_obj_scroll_to_y(wrapper, cur_y + step, LV_ANIM_ON);
            }
            lv_event_stop_processing(ev);
        } else if (key == LV_KEY_ENTER || key == LV_KEY_ESC) {
            lv_group_set_editing(g, false);
            lv_event_stop_processing(ev);
        }
    }, LV_EVENT_KEY, NULL);

    text_area = lv_label_create(scroll_wrapper);
    lv_label_set_long_mode(text_area, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text_area, LV_PCT(100));
    lv_obj_set_style_text_font(text_area, get_md_font(), 0);
    lv_obj_set_style_text_color(text_area, lv_color_white(), 0);
    lv_obj_remove_flag(text_area, LV_OBJ_FLAG_CLICKABLE);

    lbl_progress = lv_label_create(view_page);
    lv_label_set_text(lbl_progress, "0%");
    lv_obj_add_flag(lbl_progress, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(lbl_progress, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(lbl_progress, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lbl_progress, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(lbl_progress, 4, 0);
    lv_obj_set_style_pad_ver(lbl_progress, 2, 0);
    lv_obj_align(lbl_progress, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    index_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(index_page, LV_FLEX_FLOW_COLUMN);

    index_list = lv_list_create(index_page);
    lv_obj_set_width(index_list, LV_PCT(100));
    lv_obj_set_flex_grow(index_list, 1);

    lv_group_add_obj(lv_group_get_default(), scroll_wrapper);

    load_files();
    refresh_ui();

    lv_menu_set_page(menu, main_page);
    sync_menu_header();
}

void ui_news_exit(lv_obj_t *parent)
{
    ui_hide_back_button();
    disable_keyboard();
    if (menu) {
        lv_obj_del(menu);
        menu = NULL;
    }
    file_list = NULL;
    main_page = NULL;
    view_page = NULL;
    index_page = NULL;
    index_list = NULL;
    scroll_wrapper = NULL;
    text_area = NULL;
    lbl_progress = NULL;
}

} // namespace

namespace apps {
class NewsApp : public core::App {
public:
    NewsApp() : core::App("News") {}
    void onStart(lv_obj_t *parent) override { ui_news_enter(parent); }
    void onStop() override {
        ui_news_exit(getRoot());
        core::App::onStop();
    }
};

std::shared_ptr<core::App> make_news_app() {
    return std::make_shared<NewsApp>();
}
} // namespace apps
