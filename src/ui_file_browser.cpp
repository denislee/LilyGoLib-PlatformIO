/**
 * @file      ui_file_browser.cpp
 * @brief     File browser with source selection and pagination.
 */
#include "ui_define.h"
#include "core/app_manager.h"
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

static StorageSource current_source = SOURCE_INTERNAL;
static int current_page = 0;
#define FILE_PAGE_SIZE 10

static std::vector<std::string> all_files;

void load_files()
{
    all_files.clear();
    if (current_source == SOURCE_INTERNAL) {
        hw_get_internal_txt_files(all_files);
    } else {
        hw_get_sd_txt_files(all_files);
    }
    
    // Filter out tasks.txt
    all_files.erase(std::remove_if(all_files.begin(), all_files.end(),
                                   [](const std::string &s) {
                                       return s == "tasks.txt" || s == "/tasks.txt";
                                   }),
                    all_files.end());
}

static void file_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *filename = lv_list_get_button_text(file_list, btn);
    if (!filename) return;
    
    // Ensure path starts with /
    std::string path = filename;
    if (path[0] != '/') path = "/" + path;

    // File Browser is "Internal" view usually, but we need to know if it's SD or not.
    // However, the Editor app's hw_read_file handles SD fallback if preferred.
    // To be explicit, we could set the preference temporarily or pass a full path.
    // For now, we assume the Editor will find it.
    
    core::AppManager::getInstance().switchApp("Editor", parent_obj);
    ui_text_editor_open_file(path.c_str());
}

void ui_file_browser_enter(lv_obj_t *parent);
void ui_file_browser_exit(lv_obj_t *parent);

static void refresh_ui()
{
    if (!file_list) return;
    lv_obj_clean(file_list);
    
    int total_files = (int)all_files.size();
    int start_idx = current_page * FILE_PAGE_SIZE;
    int end_idx = std::min(start_idx + FILE_PAGE_SIZE, total_files);
    
    if (start_idx >= total_files && total_files > 0) {
        current_page = (total_files - 1) / FILE_PAGE_SIZE;
        start_idx = current_page * FILE_PAGE_SIZE;
        end_idx = total_files;
    }

    if (total_files == 0) {
        lv_list_add_text(file_list, "No files found");
    } else {
        // Add "Previous Page" if needed
        if (current_page > 0) {
            lv_obj_t *btn = lv_list_add_btn(file_list, LV_SYMBOL_UP, "Previous Page");
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                current_page--;
                refresh_ui();
            }, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), btn);
        }

        for (int i = start_idx; i < end_idx; ++i) {
            lv_obj_t *btn = lv_list_add_btn(file_list, LV_SYMBOL_FILE, all_files[i].c_str());
            lv_obj_add_event_cb(btn, file_click_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), btn);
        }

        // Add "Next Page" if needed
        if (end_idx < total_files) {
            lv_obj_t *btn = lv_list_add_btn(file_list, LV_SYMBOL_DOWN, "Next Page");
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                current_page++;
                refresh_ui();
            }, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), btn);
        }
        
        // Add page info
        char buf[32];
        snprintf(buf, sizeof(buf), "Page %d/%d (%d files)", 
                 current_page + 1, 
                 (total_files + FILE_PAGE_SIZE - 1) / FILE_PAGE_SIZE,
                 total_files);
        lv_list_add_text(file_list, buf);
    }
    
    // Auto-focus first item in list if possible
    if (lv_obj_get_child_count(file_list) > 0) {
        lv_group_focus_obj(lv_obj_get_child(file_list, 0));
    }
}

static void source_switch_cb(lv_event_t *e)
{
    StorageSource src = (StorageSource)(uintptr_t)lv_event_get_user_data(e);
    if (current_source != src) {
        current_source = src;
        current_page = 0;
        load_files();
        refresh_ui();
        
        // Update button styles to show selection
        lv_obj_t *btn_int = (lv_obj_t *)lv_event_get_param(e); // Passing btn_int as param if we want, but let's just find them
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

void ui_file_browser_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    menu = create_menu(parent, back_event_handler);
    lv_menu_set_mode_root_back_btn(menu, LV_MENU_ROOT_BACK_BTN_ENABLED);
    
    lv_obj_t *back_btn = lv_menu_get_main_header_back_button(menu);
    if (back_btn) lv_group_add_obj(lv_group_get_default(), back_btn);

    main_page = lv_menu_page_create(menu, (char*)"File Browser");
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);

    // Header with source selection
    lv_obj_t *header = lv_obj_create(main_page);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 2, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);

    auto create_src_btn = [&](const char *txt, StorageSource src) {
        lv_obj_t *btn = lv_btn_create(header);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, txt);
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            StorageSource s = (StorageSource)(uintptr_t)lv_event_get_user_data(e);
            if (current_source != s) {
                current_source = s;
                current_page = 0;
                load_files();
                refresh_ui();
            }
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)src);
        lv_group_add_obj(lv_group_get_default(), btn);
        return btn;
    };

    create_src_btn("Internal", SOURCE_INTERNAL);
    create_src_btn("SD Card", SOURCE_SD);

    file_list = lv_list_create(main_page);
    lv_obj_set_width(file_list, LV_PCT(100));
    lv_obj_set_flex_grow(file_list, 1);

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
