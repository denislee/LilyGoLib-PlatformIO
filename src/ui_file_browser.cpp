/**
 * @file      ui_file_browser.cpp
 * @brief     File browser with source selection, extension filter,
 *            and folder listing.
 */
#include "ui_define.h"
#include "core/app_manager.h"
#include "hal/system.h"
#include "hal/storage.h"
#include <vector>
#include <string>
#include <algorithm>

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_10);

namespace {

enum StorageSource {
    SOURCE_INTERNAL,
    SOURCE_SD
};

enum FileFilter {
    FILTER_TXT,      // show only .txt files (+ folders)
    FILTER_NON_TXT,  // show every non-.txt file (+ folders)
    FILTER_ALL       // show every file (+ folders)
};

static inline bool ends_with_ci(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return false;
    }
    return true;
}

struct Entry {
    std::string path;
    bool is_dir;
    uint32_t mtime;
    uint32_t size;
};

static lv_obj_t *menu = NULL;
static lv_obj_t *parent_obj = NULL;
static lv_obj_t *file_list = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *main_page = NULL;
static lv_obj_t *src_btn_int = NULL;
static lv_obj_t *src_btn_sd = NULL;
static lv_obj_t *filter_btn_txt = NULL;
static lv_obj_t *filter_btn_non_txt = NULL;
static lv_obj_t *filter_btn_all = NULL;

static StorageSource current_source = SOURCE_INTERNAL;
static FileFilter current_filter = FILTER_TXT;
static std::string current_dir = "/";

static std::vector<Entry> all_entries;
// Backing storage for button user_data — keeps c_str() stable while list is shown.
static std::vector<std::string> path_store;

static std::string join_path(const std::string &dir, const std::string &leaf)
{
    if (dir.empty() || dir == "/") return "/" + leaf;
    if (!dir.empty() && dir.back() == '/') return dir + leaf;
    return dir + "/" + leaf;
}

static std::string parent_of(const std::string &dir)
{
    if (dir.empty() || dir == "/") return "/";
    std::string d = dir;
    if (d.back() == '/') d.pop_back();
    size_t slash = d.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return d.substr(0, slash);
}

static void load_entries()
{
    all_entries.clear();

    std::vector<HwDirEntry> raw;
    if (current_source == SOURCE_INTERNAL) {
        hw_list_internal_entries(raw, nullptr, current_dir.c_str());
    } else {
        hw_list_sd_entries(raw, nullptr, current_dir.c_str());
    }

    for (const auto &r : raw) {
        // Hide tasks.txt bookkeeping file from the browser.
        if (!r.is_dir && (r.path == "tasks.txt" || r.path == "/tasks.txt")) continue;

        // Apply UI-side filter to files; folders are always shown.
        if (!r.is_dir) {
            bool is_txt = ends_with_ci(r.path, ".txt");
            if (current_filter == FILTER_TXT && !is_txt) continue;
            if (current_filter == FILTER_NON_TXT && is_txt) continue;
        }
        all_entries.push_back({r.path, r.is_dir, r.mtime, r.size});
    }

    std::sort(all_entries.begin(), all_entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.mtime != b.mtime) return a.mtime > b.mtime;
                  return a.path < b.path;
              });
}

static std::string format_mtime(uint32_t t)
{
    if (t == 0) return std::string("-");
    time_t tt = (time_t)t;
    struct tm info;
#ifdef ARDUINO
    localtime_r(&tt, &info);
#else
    struct tm *p = localtime(&tt);
    if (!p) return std::string("-");
    info = *p;
#endif
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             info.tm_year + 1900, info.tm_mon + 1, info.tm_mday);
    return std::string(buf);
}

static std::string format_size(uint32_t bytes)
{
    char buf[8];
    if (bytes < 1024u) {
        snprintf(buf, sizeof(buf), "%uB", (unsigned)bytes);
    } else if (bytes < 1024u * 1024u) {
        unsigned kb10 = (bytes * 10u + 512u) / 1024u;
        if (kb10 < 100u) snprintf(buf, sizeof(buf), "%u.%uK", kb10 / 10u, kb10 % 10u);
        else             snprintf(buf, sizeof(buf), "%uK", (bytes + 512u) / 1024u);
    } else {
        unsigned mb10 = (bytes * 10u + (1u << 19)) / (1u << 20);
        if (mb10 < 100u) snprintf(buf, sizeof(buf), "%u.%uM", mb10 / 10u, mb10 % 10u);
        else             snprintf(buf, sizeof(buf), "%uM", (bytes + (1u << 19)) / (1u << 20));
    }
    return std::string(buf);
}

static std::string display_name(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

static void refresh_ui();

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

static void folder_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *full = (const char *)lv_obj_get_user_data(btn);
    if (!full) return;
    current_dir = full;
    load_entries();
    refresh_ui();
}

static void parent_click_cb(lv_event_t *e)
{
    current_dir = parent_of(current_dir);
    load_entries();
    refresh_ui();
}

static void style_toggle_btn(lv_obj_t *btn, bool active)
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
    path_store.clear();

    int total = (int)all_entries.size();
    bool show_parent = (current_dir != "/" && !current_dir.empty());

    style_toggle_btn(src_btn_int,        current_source == SOURCE_INTERNAL);
    style_toggle_btn(src_btn_sd,         current_source == SOURCE_SD);
    style_toggle_btn(filter_btn_txt,     current_filter == FILTER_TXT);
    style_toggle_btn(filter_btn_non_txt, current_filter == FILTER_NON_TXT);
    style_toggle_btn(filter_btn_all,     current_filter == FILTER_ALL);

    // Reserve up-front so c_str() pointers stay stable across push_backs.
    path_store.reserve(total + (show_parent ? 1 : 0));

    if (show_parent) {
        lv_obj_t *btn = lv_list_add_btn(file_list, LV_SYMBOL_LEFT, "..");
        lv_obj_add_event_cb(btn, parent_click_cb, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(lv_group_get_default(), btn);
    }

    if (total == 0 && !show_parent) {
        lv_obj_t *empty = lv_label_create(file_list);
        lv_label_set_text(empty, LV_SYMBOL_WARNING "  No items");
        lv_obj_set_style_text_color(empty, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_pad_all(empty, 16, 0);
    } else {
        // Pre-size path_store so c_str() pointers stay valid for user_data.
        for (int i = 0; i < total; ++i) {
            path_store.push_back(join_path(current_dir, all_entries[i].path));
        }

        for (int i = 0; i < total; ++i) {
            const Entry &ent = all_entries[i];
            std::string label = display_name(ent.path);
            const char *icon = ent.is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
            lv_obj_t *btn = lv_list_add_btn(file_list, icon, label.c_str());
            lv_obj_set_user_data(btn, (void *)path_store[i].c_str());
            if (!ent.is_dir) {
                lv_obj_add_event_cb(btn, file_click_cb, LV_EVENT_CLICKED, NULL);
            } else {
                lv_obj_add_event_cb(btn, folder_click_cb, LV_EVENT_CLICKED, NULL);
            }
            lv_group_add_obj(lv_group_get_default(), btn);

            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            lv_obj_t *icon_obj = lv_obj_get_child(btn, 0);
            lv_obj_t *lbl = lv_obj_get_child(btn, 1);
            if (icon_obj) {
                lv_obj_set_width(icon_obj, 20);
                lv_obj_set_style_text_align(icon_obj, LV_TEXT_ALIGN_CENTER, 0);
            }
            if (lbl) {
                lv_obj_set_width(lbl, 0);
                lv_obj_set_flex_grow(lbl, 1);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            }

            // Trailing columns: size (files only) then last-modified date.
            if (!ent.is_dir) {
                lv_obj_t *size_lbl = lv_label_create(btn);
                lv_label_set_text(size_lbl, format_size(ent.size).c_str());
                lv_obj_set_style_text_color(size_lbl, UI_COLOR_MUTED, 0);
                lv_obj_set_style_text_font(size_lbl, &lv_font_montserrat_10, 0);
                lv_obj_set_style_pad_left(size_lbl, 8, 0);
            }
            lv_obj_t *date_lbl = lv_label_create(btn);
            std::string date_txt = format_mtime(ent.mtime);
            lv_label_set_text(date_lbl, date_txt.c_str());
            lv_obj_set_style_text_color(date_lbl, UI_COLOR_MUTED, 0);
            lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_pad_left(date_lbl, 8, 0);
        }
    }

    lv_obj_t *to_focus = NULL;
    if ((total > 0 || show_parent) && lv_obj_get_child_count(file_list) > 0) {
        to_focus = lv_obj_get_child(file_list, 0);
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
        current_dir = "/";
        load_entries();
    }
    refresh_ui();
}

static void filter_click_cb(lv_event_t *e)
{
    FileFilter f = (FileFilter)(uintptr_t)lv_event_get_user_data(e);
    if (current_filter != f) {
        current_filter = f;
        load_entries();
    }
    refresh_ui();
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

static lv_obj_t *make_side_btn(lv_obj_t *parent, const char *icon, const char *txt,
                               lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(btn, UI_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(btn);
    if (icon && icon[0]) {
        lv_label_set_text_fmt(lbl, "%s %s", icon, txt);
    } else {
        lv_label_set_text(lbl, txt);
    }
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_group_add_obj(lv_group_get_default(), btn);
    return btn;
}

static lv_obj_t *make_divider(lv_obj_t *parent)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_width(line, LV_PCT(70));
    lv_obj_set_height(line, 1);
    lv_obj_set_style_bg_color(line, UI_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_margin_ver(line, 2, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    return line;
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
            load_entries();
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

    const char *full = (const char *)lv_obj_get_user_data(focused);
    if (!full) return;

    std::string path_str = full;
    for (const auto &ent : all_entries) {
        if (join_path(current_dir, ent.path) == path_str) {
            if (!ent.is_dir) show_delete_confirm(path_str);
            return;
        }
    }
}

void ui_file_browser_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    bool sd_online = (HW_SD_ONLINE & hw_get_device_online()) != 0;
    if (!sd_online && current_source == SOURCE_SD) current_source = SOURCE_INTERNAL;
    current_dir = "/";

    menu = create_menu(parent, back_event_handler);

    main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(main_page, 0, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    ui_show_back_button(exit_btn_cb);

    // Body: two frames side-by-side — left sidebar (controls), right file list.
    lv_obj_t *body = lv_obj_create(main_page);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_column(body, 6, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // Left sidebar: storage + filter stacked vertically.
    lv_obj_t *sidebar = lv_obj_create(body);
    lv_obj_set_width(sidebar, LV_PCT(22));
    lv_obj_set_height(sidebar, LV_PCT(100));
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sidebar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(sidebar, 4, 0);
    lv_obj_set_style_pad_row(sidebar, 3, 0);
    lv_obj_set_style_border_width(sidebar, 1, 0);
    lv_obj_set_style_border_color(sidebar, UI_COLOR_MUTED, 0);
    lv_obj_set_style_radius(sidebar, UI_RADIUS, 0);
    lv_obj_set_style_bg_opa(sidebar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(sidebar, &lv_font_montserrat_14, 0);

    src_btn_int = make_side_btn(sidebar, LV_SYMBOL_DRIVE, "Internal",
                                source_click_cb, (void *)(uintptr_t)SOURCE_INTERNAL);
    if (sd_online) {
        src_btn_sd = make_side_btn(sidebar, LV_SYMBOL_SD_CARD, "SD Card",
                                   source_click_cb, (void *)(uintptr_t)SOURCE_SD);
    } else {
        src_btn_sd = NULL;
    }

    make_divider(sidebar);
    filter_btn_txt = make_side_btn(sidebar, NULL, ".txt only",
                                   filter_click_cb, (void *)(uintptr_t)FILTER_TXT);
    filter_btn_non_txt = make_side_btn(sidebar, NULL, "No .txt",
                                       filter_click_cb, (void *)(uintptr_t)FILTER_NON_TXT);
    filter_btn_all = make_side_btn(sidebar, NULL, "All",
                                   filter_click_cb, (void *)(uintptr_t)FILTER_ALL);

    // Right side: file list.
    file_list = lv_list_create(body);
    lv_obj_set_flex_grow(file_list, 1);
    lv_obj_set_height(file_list, LV_PCT(100));
    lv_obj_set_style_radius(file_list, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(file_list, 2, 0);
    lv_obj_add_event_cb(file_list, file_list_key_cb, LV_EVENT_KEY, NULL);

    load_entries();
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
    filter_btn_txt = NULL;
    filter_btn_non_txt = NULL;
    filter_btn_all = NULL;
    path_store.clear();
    all_entries.clear();
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
