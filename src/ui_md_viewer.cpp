/**
 * @file      ui_md_viewer.cpp
 * @brief     Markdown file viewer from SD card.
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

static lv_obj_t *btn_index = NULL;
static lv_obj_t *lbl_progress = NULL;

static bool has_prev_page = false;
static bool has_next_page = false;

static std::vector<std::string> md_files;
static std::vector<std::pair<std::string, size_t>> md_headers;

static std::string current_md_path = "";
static size_t current_md_size = 0;
static size_t current_md_offset = 0;
static size_t current_page_len = 0; // length of the chunk rendered for current page
static const size_t MD_CHUNK_SIZE = 2048; // Load 2KB at a time
static std::vector<size_t> page_offsets;
static bool md_headers_loaded = false;

void ui_md_viewer_exit(lv_obj_t *parent);
static void refresh_ui();
static void sync_menu_header();

static std::string sanitize_utf8(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (i + 2 < input.size() && (unsigned char)input[i] == 0xE2 && (unsigned char)input[i+1] == 0x80) {
            unsigned char c = (unsigned char)input[i+2];
            if (c == 0x98 || c == 0x99) { out += '\''; i += 2; continue; } // Smart single quotes
            else if (c == 0x9C || c == 0x9D) { out += '"'; i += 2; continue; } // Smart double quotes
            else if (c == 0x93 || c == 0x94) { out += '-'; i += 2; continue; } // En/Em dashes
            else if (c == 0xA6) { out += "..."; i += 2; continue; } // Ellipsis
        }
        out += input[i];
    }
    return out;
}

static void emit_line(std::string &out, const char *line, size_t len) {
    bool is_header = (len >= 1 && line[0] == '#');
    bool is_quote  = (len >= 2 && line[0] == '>' && line[1] == ' ');

    if (is_header)      out.append("#00bfff ", 8);
    else if (is_quote)  out.append("#808080 ", 8);

    bool in_bold = false;
    for (size_t j = 0; j < len; ++j) {
        char c = line[j];
        if (!is_header && !is_quote && c == '*' && j + 1 < len && line[j+1] == '*') {
            in_bold = !in_bold;
            if (in_bold) out.append("#ff8c00 ", 8);
            else         out.push_back('#');
            ++j;
        } else if (!is_header && !is_quote && c == '`') {
            out.append("#32cd32 ", 8);
            size_t end = j + 1;
            while (end < len && line[end] != '`') ++end;
            for (size_t k = j + 1; k < end; ++k) {
                if (line[k] == '#') out.append("##", 2);
                else                out.push_back(line[k]);
            }
            out.push_back('#');
            j = (end < len) ? end : len - 1;
        } else if (c == '#') {
            out.append("##", 2);
        } else {
            out.push_back(c);
        }
    }

    if (is_header || is_quote) out.push_back('#');
}

static std::string parse_markdown(const std::string& input) {
    std::string sanitized = sanitize_utf8(input);
    std::string out;
    out.reserve(sanitized.size() + (sanitized.size() >> 2));

    size_t n = sanitized.size();
    size_t line_start = 0;
    // Strip '\r' via a small scratch only when present; common case bypasses it.
    std::string scratch;
    for (size_t i = 0; i <= n; ++i) {
        bool at_end = (i == n);
        char c = at_end ? '\n' : sanitized[i];
        if (c == '\n') {
            size_t raw_len = i - line_start;
            const char *line_ptr;
            size_t line_len;
            // Check if line contains '\r' — if not, use the raw span.
            bool has_cr = false;
            for (size_t k = line_start; k < i; ++k) {
                if (sanitized[k] == '\r') { has_cr = true; break; }
            }
            if (has_cr) {
                scratch.clear();
                scratch.reserve(raw_len);
                for (size_t k = line_start; k < i; ++k) {
                    if (sanitized[k] != '\r') scratch.push_back(sanitized[k]);
                }
                line_ptr = scratch.data();
                line_len = scratch.size();
            } else {
                line_ptr = sanitized.data() + line_start;
                line_len = raw_len;
            }
            emit_line(out, line_ptr, line_len);
            if (!at_end) out.push_back('\n');
            line_start = i + 1;
        }
    }
    return out;
}

static void load_files()
{
    md_files.clear();
    hw_get_sd_md_files(md_files);
}

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        ui_md_viewer_exit(NULL);
        menu_show();
    }
}

// Pages are navigated via lv_menu_set_page directly, which doesn't populate
// lv_menu's history stack. Handle back navigation explicitly based on the
// currently-displayed page.
static void md_view_back_cb(lv_event_t *e)
{
    if (!menu) return;
    lv_obj_t *cur = lv_menu_get_cur_main_page(menu);
    if (cur == view_page) {
        // If this file has an index, return to it; otherwise back to file list.
        lv_obj_t *target = (md_headers_loaded && !md_headers.empty()) ? index_page : main_page;
        lv_menu_set_page(menu, target);
        sync_menu_header();
    } else if (cur == index_page) {
        lv_menu_set_page(menu, main_page);
        sync_menu_header();
    } else {
        ui_md_viewer_exit(NULL);
        menu_show();
    }
}

// Hide the menu's built-in header so the status bar back button is the only
// "<" visible. Called after each page change — only needed to re-apply the
// hidden flag if lv_menu restored it.
static void sync_menu_header()
{
    if (!menu) return;
    lv_obj_t *header = lv_menu_get_main_header(menu);
    if (header) lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);
}

static void update_page()
{
    if (current_md_path.empty()) return;

    std::string content;
    if (!hw_read_file_chunk(current_md_path.c_str(), current_md_offset, MD_CHUNK_SIZE, content)) {
        ui_msg_pop_up("Error", "Failed to read file.");
        return;
    }

    current_page_len = content.length();
    std::string parsed = parse_markdown(content);
    lv_label_set_text(text_area, parsed.c_str());

    // Scroll to top
    lv_obj_scroll_to_y(scroll_wrapper, 0, LV_ANIM_OFF);

    size_t next_offset = current_md_offset + current_page_len;

    // Update progress
    int percent = 0;
    if (current_md_size > 0) {
        percent = (int)(((uint64_t)next_offset * 100) / current_md_size);
    } else {
        percent = 100;
    }
    
    lv_label_set_text_fmt(lbl_progress, "%d%%", percent);

    // Update pagination flags
    has_prev_page = (current_md_offset > 0);
    has_next_page = (next_offset < current_md_size);
    
    lv_group_focus_obj(scroll_wrapper); // Focus wrapper so user can immediately scroll text
    lv_group_set_editing(lv_group_get_default(), true); // Enter editing mode immediately to maintain scroll control
}

static void prev_btn_cb(lv_event_t *e)
{
    if (page_offsets.size() > 1) {
        page_offsets.pop_back(); // Remove current
        current_md_offset = page_offsets.back(); // Get previous
        update_page();
        
        // When going back, scroll to the bottom of the new page to maintain continuity
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
    current_md_offset += current_page_len;
    page_offsets.push_back(current_md_offset);
    update_page();
}

static void index_click_cb(lv_event_t *e)
{
    size_t offset = (size_t)lv_event_get_user_data(e);

    current_md_offset = offset;
    page_offsets.clear();
    page_offsets.push_back(current_md_offset);

    lv_menu_set_page(menu, view_page);
    sync_menu_header();
    update_page();
}

static bool index_progress_cb(size_t current, size_t total)
{
    lv_timer_handler();
    return true;
}

static void btn_index_click_cb(lv_event_t *e)
{
    if (!md_headers_loaded) {
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

        hw_get_md_headers(current_md_path.c_str(), md_headers, index_progress_cb);

        md_headers_loaded = true;
        lv_obj_del(loading_cont);
    }

    if (md_headers.empty()) {
        if (e == NULL) {
            lv_menu_set_page(menu, view_page);
            sync_menu_header();
            update_page();
        } else {
            ui_msg_pop_up("Index", "No headers found in this file.");
        }
        return;
    }

    lv_obj_clean(index_list);
    for (size_t i = 0; i < md_headers.size(); i++) {
        std::string safe_title = sanitize_utf8(md_headers[i].first);
        lv_obj_t *btn = lv_list_add_btn(index_list, NULL, safe_title.c_str());
        lv_obj_add_event_cb(btn, index_click_cb, LV_EVENT_CLICKED, (void*)md_headers[i].second);

        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_obj_set_width(lbl, 0);
            lv_obj_set_flex_grow(lbl, 1);
            
            const lv_font_t * font = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
            if (font) {
                lv_obj_set_height(lbl, lv_font_get_line_height(font));
            }
            
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        }
        
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(ev);
            lv_obj_t *l = lv_obj_get_child(b, 1);
            if (l) lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
        }, LV_EVENT_FOCUSED, NULL);
        
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(ev);
            lv_obj_t *l = lv_obj_get_child(b, 1);
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

static std::string resolve_md_path(const char *filename)
{
    std::string path = filename;
    if (path.empty()) return path;
    if (path[0] != '/') path = "/" + path;
    if (path.find("/md/") != 0) {
        if (path[0] == '/') path = "/md" + path;
        else path = "/md/" + path;
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

    show_delete_confirm(resolve_md_path(filename));
}

static void file_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *filename = lv_list_get_button_text(file_list, btn);
    if (!filename) return;

    std::string path = resolve_md_path(filename);

    current_md_path = path;
    current_md_size = hw_get_file_size(current_md_path.c_str());
    md_headers.clear();
    md_headers_loaded = false;
    current_md_offset = 0;
    current_page_len = 0;
    page_offsets.clear();
    page_offsets.push_back(0);
    
    // Instead of viewing immediately, navigate to the index page first
    btn_index_click_cb(NULL);
}

static void refresh_ui()
{
    if (!file_list) return;
    lv_obj_clean(file_list);

    if (md_files.empty()) {
        lv_list_add_text(file_list, "No .md files found in /md");
    } else {
        for (const auto &file : md_files) {
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

void ui_md_viewer_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    menu = create_menu(parent, back_event_handler);
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_ENABLED);

    // Suppress the built-in header back button: zero-size and transparent so
    // the menu header's content_height stays 0 and LVGL auto-hides the
    // header. Still clickable programmatically via send_event.
    lv_obj_t *bb = lv_menu_get_main_header_back_button(menu);
    if (bb) {
        lv_obj_set_size(bb, 0, 0);
        lv_obj_set_style_pad_all(bb, 0, 0);
        lv_obj_set_style_border_width(bb, 0, 0);
        lv_obj_set_style_outline_width(bb, 0, 0);
        lv_obj_set_style_shadow_width(bb, 0, 0);
        lv_obj_set_style_bg_opa(bb, LV_OPA_TRANSP, 0);
    }

    // Route the single status bar back button through the menu's built-in back
    // so the menu's back_event_handler decides root-vs-subpage behaviour.
    ui_show_back_button(md_view_back_cb);

    main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);

    file_list = lv_list_create(main_page);
    lv_obj_set_width(file_list, LV_PCT(100));
    lv_obj_set_flex_grow(file_list, 1);
    lv_obj_add_event_cb(file_list, file_list_key_cb, LV_EVENT_KEY, NULL);

    view_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(view_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(view_page, 0, 0);

    // Create a wrapper for the text area that is focusable and scrollable
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
        // LV_KEY_UP / LV_KEY_LEFT: move toward the start of content (scroll up).
        // Use lv_obj_scroll_to_y so we issue an absolute target — avoids a
        // race with LVGL's default arrow-key scroll handler, which also
        // re-runs after ours and would otherwise half-cancel the movement.
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
    lv_label_set_recolor(text_area, true);
    lv_label_set_long_mode(text_area, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text_area, LV_PCT(100));
    lv_obj_set_style_text_font(text_area, get_md_font(), 0);
    lv_obj_set_style_text_color(text_area, lv_color_white(), 0);
    lv_obj_remove_flag(text_area, LV_OBJ_FLAG_CLICKABLE);

    // Back button moves to the top status bar; visibility is driven by
    // sync_menu_header() based on which menu page is active.

    // Progress overlay in bottom-right corner of the viewer
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

    // Group for physical buttons
    lv_group_add_obj(lv_group_get_default(), scroll_wrapper); // Enable text scrolling

    load_files();
    refresh_ui();

    lv_menu_set_page(menu, main_page);
    sync_menu_header();
}

void ui_md_viewer_exit(lv_obj_t *parent)
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
    btn_index = NULL;
    lbl_progress = NULL;
}

} // namespace

namespace apps {
class MdViewerApp : public core::App {
public:
    MdViewerApp() : core::App("MD Viewer") {}
    void onStart(lv_obj_t *parent) override { ui_md_viewer_enter(parent); }
    void onStop() override {
        ui_md_viewer_exit(getRoot());
        core::App::onStop();
    }
};

std::shared_ptr<core::App> make_md_viewer_app() {
    return std::make_shared<MdViewerApp>();
}
} // namespace apps