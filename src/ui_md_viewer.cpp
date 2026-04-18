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

void ui_md_viewer_exit(lv_obj_t *parent);

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
    update_page();
}

static bool md_headers_loaded = false;
static bool cancel_index_loading = false;

static void btn_cancel_index_cb(lv_event_t *e)
{
    cancel_index_loading = true;
}

static bool index_progress_cb(size_t current, size_t total)
{
    lv_timer_handler(); // Process LVGL events to catch button clicks
    return !cancel_index_loading;
}

static void btn_index_click_cb(lv_event_t *e)
{
    if (!md_headers_loaded) {
        cancel_index_loading = false;

        lv_obj_t *loading_cont = lv_obj_create(lv_screen_active());
        lv_obj_set_size(loading_cont, 200, 120);
        lv_obj_center(loading_cont);
        lv_obj_set_style_bg_color(loading_cont, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(loading_cont, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(loading_cont, lv_color_white(), 0);
        lv_obj_set_style_border_width(loading_cont, 2, 0);
        lv_obj_set_flex_flow(loading_cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(loading_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *loading_label = lv_label_create(loading_cont);
        lv_label_set_text(loading_label, "Loading indexes...");

        lv_obj_t *cancel_btn = lv_btn_create(loading_cont);
        lv_obj_t *cancel_label = lv_label_create(cancel_btn);
        lv_label_set_text(cancel_label, "Cancel");
        lv_obj_add_event_cb(cancel_btn, btn_cancel_index_cb, LV_EVENT_CLICKED, NULL);

        lv_group_add_obj(lv_group_get_default(), cancel_btn);
        lv_group_focus_obj(cancel_btn);

        lv_refr_now(NULL);

        hw_get_md_headers(current_md_path.c_str(), md_headers, index_progress_cb);

        md_headers_loaded = !cancel_index_loading;
        lv_obj_del(loading_cont);

        if (cancel_index_loading) {
            // User cancelled, clear any partially loaded headers and return without showing the index menu
            md_headers.clear();
            return;
        }
    }

    if (md_headers.empty()) {
        if (e == NULL) {
            lv_menu_set_page(menu, view_page);
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
}

static void file_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *filename = lv_list_get_button_text(file_list, btn);
    if (!filename) return;

    std::string path = filename;
    if (path[0] != '/') path = "/" + path;
    if (path.find("/md/") != 0) {
        if (path[0] == '/') path = "/md" + path;
        else path = "/md/" + path;
    }

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

    lv_obj_t *back_btn = lv_menu_get_main_header_back_button(menu);
    if (back_btn) lv_group_add_obj(lv_group_get_default(), back_btn);

    main_page = lv_menu_page_create(menu, (char*)"MD Viewer");
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);

    file_list = lv_list_create(main_page);
    lv_obj_set_width(file_list, LV_PCT(100));
    lv_obj_set_flex_grow(file_list, 1);

    view_page = lv_menu_page_create(menu, (char*)"View MD");
    lv_obj_set_flex_flow(view_page, LV_FLEX_FLOW_COLUMN);
    
    // Create a wrapper for the text area that is focusable and scrollable
    scroll_wrapper = lv_obj_create(view_page);
    lv_obj_set_width(scroll_wrapper, LV_PCT(100));
    lv_obj_set_flex_grow(scroll_wrapper, 1);
    lv_obj_set_scroll_dir(scroll_wrapper, LV_DIR_VER);
    lv_obj_add_flag(scroll_wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scroll_wrapper, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_border_width(scroll_wrapper, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(scroll_wrapper, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_FOCUSED);
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
                if (step > 0) lv_obj_scroll_by(wrapper, 0, step, LV_ANIM_ON);
            }
        } else if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) {
            int32_t bottom = lv_obj_get_scroll_bottom(wrapper);
            if (bottom == 0 && has_next_page) {
                next_btn_cb(NULL);
            } else {
                int32_t step = 40;
                if (bottom < step) step = bottom;
                if (step > 0) lv_obj_scroll_by(wrapper, 0, -step, LV_ANIM_ON);
            }
        } else if (key == LV_KEY_ENTER || key == LV_KEY_ESC) {
            lv_group_set_editing(g, false);
        }
    }, LV_EVENT_KEY, NULL);

    text_area = lv_label_create(scroll_wrapper);
    lv_label_set_recolor(text_area, true);
    lv_label_set_long_mode(text_area, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text_area, LV_PCT(100));
    lv_obj_set_style_text_font(text_area, get_md_font(), 0);
    lv_obj_set_style_text_color(text_area, lv_color_white(), 0);
    lv_obj_remove_flag(text_area, LV_OBJ_FLAG_CLICKABLE);

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

    index_page = lv_menu_page_create(menu, (char*)"Index");
    lv_obj_set_flex_flow(index_page, LV_FLEX_FLOW_COLUMN);
    
    index_list = lv_list_create(index_page);
    lv_obj_set_width(index_list, LV_PCT(100));
    lv_obj_set_flex_grow(index_list, 1);

    // Group for physical buttons
    lv_group_add_obj(lv_group_get_default(), scroll_wrapper); // Enable text scrolling

    load_files();
    refresh_ui();

    lv_menu_set_page(menu, main_page);
}

void ui_md_viewer_exit(lv_obj_t *parent)
{
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