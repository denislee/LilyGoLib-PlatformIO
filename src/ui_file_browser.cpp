/**
 * @file      ui_file_browser.cpp
 * @brief     File browser with source selection and pagination.
 */
#include "ui_define.h"
#include "core/app_manager.h"
#include "hal/system.h"
#include <vector>
#include <string>
#include <algorithm>

namespace {

enum StorageSource {
    SOURCE_INTERNAL,
    SOURCE_SD
};

static lv_obj_t *menu = NULL;
static lv_obj_t *parent_obj = NULL;
static lv_obj_t *file_list = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *main_page = NULL;
static lv_obj_t *src_btn_int = NULL;
static lv_obj_t *src_btn_sd = NULL;
static lv_obj_t *footer = NULL;
static lv_obj_t *prev_btn = NULL;
static lv_obj_t *next_btn = NULL;
static lv_obj_t *page_label = NULL;

static StorageSource current_source = SOURCE_INTERNAL;
static int current_page = 0;
#define FILE_PAGE_SIZE 10

static std::vector<std::string> all_files;

static void load_files()
{
    all_files.clear();
    if (current_source == SOURCE_INTERNAL) {
        hw_get_internal_txt_files(all_files);
    } else {
        hw_get_sd_txt_files(all_files);
    }

    all_files.erase(std::remove_if(all_files.begin(), all_files.end(),
                                   [](const std::string &s) {
                                       return s == "tasks.txt" || s == "/tasks.txt";
                                   }),
                    all_files.end());

    std::sort(all_files.begin(), all_files.end());
}

static std::string display_name(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

static void file_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *full = (const char *)lv_obj_get_user_data(btn);
    if (!full) return;

    std::string path = full;
    if (path.empty()) return;
    if (path[0] != '/') path = "/" + path;

    core::AppManager::getInstance().switchApp("Editor", parent_obj);
    ui_text_editor_open_file(path.c_str());
}

static void style_src_btn(lv_obj_t *btn, bool active)
{
    if (!btn) return;
    if (active) {
        lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, UI_COLOR_FG, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, UI_COLOR_MUTED, LV_PART_MAIN);
    }
}

void ui_file_browser_enter(lv_obj_t *parent);
void ui_file_browser_exit(lv_obj_t *parent);

static void refresh_ui()
{
    if (!file_list) return;
    lv_obj_clean(file_list);

    int total_files = (int)all_files.size();
    int total_pages = (total_files + FILE_PAGE_SIZE - 1) / FILE_PAGE_SIZE;
    if (total_pages < 1) total_pages = 1;

    if (current_page >= total_pages) current_page = total_pages - 1;
    if (current_page < 0) current_page = 0;

    int start_idx = current_page * FILE_PAGE_SIZE;
    int end_idx = std::min(start_idx + FILE_PAGE_SIZE, total_files);

    style_src_btn(src_btn_int, current_source == SOURCE_INTERNAL);
    style_src_btn(src_btn_sd,  current_source == SOURCE_SD);

    if (total_files == 0) {
        lv_obj_t *empty = lv_label_create(file_list);
        lv_label_set_text(empty,
            LV_SYMBOL_WARNING "  No text files found\n"
            "Create a .txt file with the Editor.");
        lv_obj_set_style_text_color(empty, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_pad_all(empty, 16, 0);
    } else {
        for (int i = start_idx; i < end_idx; ++i) {
            const std::string &full = all_files[i];
            std::string label = display_name(full);
            lv_obj_t *btn = lv_list_add_btn(file_list, LV_SYMBOL_FILE, label.c_str());
            lv_obj_set_user_data(btn, (void *)full.c_str());
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

    // Pagination footer: show only when there is more than one page.
    bool multi_page = total_pages > 1;
    lv_obj_t *to_focus = NULL;

    if (multi_page) {
        lv_obj_remove_flag(footer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(next_btn, LV_OBJ_FLAG_HIDDEN);

        if (current_page == 0) {
            lv_obj_add_state(prev_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(prev_btn, LV_STATE_DISABLED);
        }
        if (current_page >= total_pages - 1) {
            lv_obj_add_state(next_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(next_btn, LV_STATE_DISABLED);
        }

        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", current_page + 1, total_pages);
        lv_label_set_text(page_label, buf);

        lv_group_remove_obj(prev_btn);
        lv_group_remove_obj(next_btn);
        lv_group_add_obj(lv_group_get_default(), prev_btn);
        lv_group_add_obj(lv_group_get_default(), next_btn);
    } else {
        lv_obj_add_flag(footer, LV_OBJ_FLAG_HIDDEN);
        lv_group_remove_obj(prev_btn);
        lv_group_remove_obj(next_btn);
    }

    if (lv_obj_get_child_count(file_list) > 0) {
        to_focus = lv_obj_get_child(file_list, 0);
        // Skip past the empty-state label, which isn't focusable.
        if (total_files == 0) to_focus = NULL;
    }
    if (to_focus) {
        lv_group_focus_obj(to_focus);
    } else if (src_btn_int) {
        lv_group_focus_obj(current_source == SOURCE_INTERNAL ? src_btn_int : src_btn_sd);
    }
}

static void source_click_cb(lv_event_t *e)
{
    StorageSource s = (StorageSource)(uintptr_t)lv_event_get_user_data(e);
    if (current_source != s) {
        current_source = s;
        current_page = 0;
        load_files();
    }
    refresh_ui();
}

static void prev_click_cb(lv_event_t *e)
{
    if (current_page > 0) {
        current_page--;
        refresh_ui();
    }
}

static void next_click_cb(lv_event_t *e)
{
    int total_pages = ((int)all_files.size() + FILE_PAGE_SIZE - 1) / FILE_PAGE_SIZE;
    if (current_page < total_pages - 1) {
        current_page++;
        refresh_ui();
    }
}

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        ui_file_browser_exit(NULL);
        menu_show();
    }
}

static void exit_btn_cb(lv_event_t *e)
{
    ui_file_browser_exit(NULL);
    menu_show();
}

static lv_obj_t *make_source_btn(lv_obj_t *parent, const char *icon, const char *txt, StorageSource src)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, UI_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text_fmt(lbl, "%s %s", icon, txt);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, source_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)src);
    lv_group_add_obj(lv_group_get_default(), btn);
    return btn;
}

static lv_obj_t *make_nav_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_style_radius(btn, UI_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_palette_darken(LV_PALETTE_GREY, 3),
                                LV_PART_MAIN | LV_STATE_DISABLED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void ui_file_browser_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    bool sd_online = (HW_SD_ONLINE & hw_get_device_online()) != 0;
    if (!sd_online && current_source == SOURCE_SD) current_source = SOURCE_INTERNAL;

    menu = create_menu(parent, back_event_handler);

    main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(main_page, 0, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    // Back button lives on the status bar.
    ui_show_back_button(exit_btn_cb);

    // Header row: source selector only (back button moved to status bar).
    lv_obj_t *header = lv_obj_create(main_page);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_style_pad_column(header, 6, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Source selector — segmented control, fills the header row.
    lv_obj_t *src_bar = lv_obj_create(header);
    lv_obj_set_flex_grow(src_bar, 1);
    lv_obj_set_height(src_bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(src_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(src_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(src_bar, 2, 0);
    lv_obj_set_style_pad_column(src_bar, 6, 0);
    lv_obj_set_style_border_width(src_bar, 1, 0);
    lv_obj_set_style_border_color(src_bar, UI_COLOR_MUTED, 0);
    lv_obj_set_style_radius(src_bar, UI_RADIUS, 0);
    lv_obj_set_style_bg_opa(src_bar, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(src_bar, LV_OBJ_FLAG_SCROLLABLE);

    src_btn_int = make_source_btn(src_bar, LV_SYMBOL_DRIVE, "Internal", SOURCE_INTERNAL);
    if (sd_online) {
        src_btn_sd = make_source_btn(src_bar, LV_SYMBOL_SD_CARD, "SD Card", SOURCE_SD);
    } else {
        src_btn_sd = NULL;
    }

    // File list — fills remaining space.
    file_list = lv_list_create(main_page);
    lv_obj_set_width(file_list, LV_PCT(100));
    lv_obj_set_flex_grow(file_list, 1);
    lv_obj_set_style_radius(file_list, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(file_list, 2, 0);

    // Pagination footer — hidden unless >1 page.
    footer = lv_obj_create(main_page);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(footer, 2, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    prev_btn = make_nav_btn(footer, LV_SYMBOL_LEFT " Prev", prev_click_cb);

    page_label = lv_label_create(footer);
    lv_label_set_text(page_label, "1 / 1");
    lv_obj_set_style_text_color(page_label, UI_COLOR_MUTED, 0);

    next_btn = make_nav_btn(footer, "Next " LV_SYMBOL_RIGHT, next_click_cb);

    load_files();
    refresh_ui();

    lv_menu_set_page(menu, main_page);

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        ui_file_browser_exit(NULL);
        menu_show();
    }, NULL);
#endif
}

void ui_file_browser_exit(lv_obj_t *parent)
{
    ui_hide_back_button();
    disable_keyboard();
    if (menu) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
    }
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    file_list = NULL;
    main_page = NULL;
    src_btn_int = NULL;
    src_btn_sd = NULL;
    footer = NULL;
    prev_btn = NULL;
    next_btn = NULL;
    page_label = NULL;
}

} // namespace

namespace apps {
class FileBrowserApp : public core::App {
public:
    FileBrowserApp() : core::App("Files") {}
    void onStart(lv_obj_t *parent) override { ui_file_browser_enter(parent); }
    void onStop() override {
        ui_file_browser_exit(getRoot());
        core::App::onStop();
    }
};

std::shared_ptr<core::App> make_file_browser_app() {
    return std::make_shared<FileBrowserApp>();
}
} // namespace apps
