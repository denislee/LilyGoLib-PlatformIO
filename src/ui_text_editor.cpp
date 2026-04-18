/**
 * @file      ui_text_editor.cpp
 * @author    LilyGo CLI Agent
 * @license   MIT
 * @date      2026-04-10
 *
 */
#include "ui_define.h"

LV_FONT_DECLARE(lv_font_montserrat_12);

static lv_obj_t *menu = NULL;
static lv_obj_t *text_area = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *exit_cont = NULL;
static lv_obj_t *word_count_label = NULL;
static string current_file_path = "";
static lv_timer_t *autosave_timer = NULL;
static bool content_dirty = false;

void ui_text_editor_exit(lv_obj_t *parent);

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
    
    lv_label_set_text_fmt(word_count_label, "%zu words | %zu chars%s", words, chars, content_dirty ? " *" : "");
}

static bool save_content(bool is_autosave = false)
{
    if (!text_area) return true;

    if (is_autosave && !content_dirty) {
        return true; // Nothing to save
    }

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
        return true;
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
    string err;
    if (current_file_path.empty()) {
        // New files honour the user's storage preference (internal vs SD).
        success = hw_save_preferred_file(target_path.c_str(), txt, &err);
    } else {
        // Existing files: keep system files (tasks) on whichever
        // storage the preference selects; other files use the smart save which
        // prefers whichever storage already holds the file.
        if (target_path == "/tasks.txt" || (target_path.length() > 1 && isdigit(target_path[1]))) {
            success = hw_save_preferred_file(target_path.c_str(), txt, &err);
        } else {
            success = hw_save_file(target_path.c_str(), txt, &err);
        }
    }

    if (success) {
        printf("Save successful\n");
        content_dirty = false;
        if (current_file_path.empty()) {
            current_file_path = target_path;
        }
        update_word_count(); // to clear the asterisk
        return true;
    } else {
        printf("Save failed: %s\n", err.c_str());
        if (!is_autosave) {
            string msg = "Failed to save file!";
            if (!err.empty()) {
                msg += "\n";
                msg += err;
            }
            msg += "\nPath: ";
            msg += target_path;
            ui_msg_pop_up("Save", msg.c_str());
        }
        return false;
    }
}

static void autosave_timer_cb(lv_timer_t *t)
{
    save_content(true);
}

static void do_exit()
{
    // If save fails, stay in the editor so the "Save failed" msgbox remains
    // interactable. menu_show() re-binds input devices to the menu group, which
    // would otherwise leave the msgbox's Close button unreachable via encoder/keyboard.
    if (!save_content(false)) {
        return;
    }
    // menu_show will trigger AppManager::switchApp which calls ui_text_editor_exit
    menu_show();
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
        content_dirty = true;
        update_word_count();
    } else if (code == LV_EVENT_CLICKED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev && lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) {
            // Only toggle on click if it's NOT the encoder (e.g., touchscreen)
            // Encoder toggle is handled in LV_EVENT_KEY to prevent newline insertion
            bool editing = lv_group_get_editing(g);
            lv_group_set_editing(g, !editing);
        }
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        lv_indev_t *indev = lv_indev_get_act();
        bool editing = lv_group_get_editing(g);

        if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER && key == LV_KEY_ENTER) {
            lv_group_set_editing(g, !editing);
            if (editing) {
                // Remove the newline character inserted by the default textarea handler
                lv_textarea_delete_char((lv_obj_t *)lv_event_get_target(e));
            }
            lv_event_stop_processing(e);
            return;
        }

        if (key == 0x1E) {
            lv_event_stop_processing(e);
            return;
        }
        if (key == LV_KEY_ESC) {
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
    // If it was already partially active or pointers are stale, clean up
    if (menu != NULL) {
        // If we are already here, don't do anything
        if (lv_obj_get_parent(menu) == parent) {
            return;
        }
        ui_text_editor_exit(NULL);
    }
    enable_keyboard();

    content_dirty = false;
    if (autosave_timer) {
        lv_timer_del(autosave_timer);
    }
    autosave_timer = lv_timer_create(autosave_timer_cb, 60000, NULL);

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
    lv_obj_set_style_text_font(word_count_label, &lv_font_montserrat_12, 0);

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

    lv_group_t *g = lv_obj_get_group(text_area);
    if (g) {
        lv_group_focus_obj(text_area);
        lv_group_set_editing(g, editor_auto_edit);
        editor_auto_edit = false;
    }
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
        content_dirty = false;
        update_word_count();
    }
}

void ui_text_editor_new_document()
{
    if (text_area == NULL) return;
    if (!save_content(false)) {
        // Don't discard current content if save failed.
        return;
    }
    lv_textarea_set_text(text_area, "");
    current_file_path = "";
    content_dirty = false;
    
    lv_group_t *g = lv_obj_get_group(text_area);
    if (g) {
        lv_group_focus_obj(text_area);
        lv_group_set_editing(g, editor_auto_edit);
        editor_auto_edit = false;
    }
    
    update_word_count();
}

void ui_text_editor_exit(lv_obj_t *parent)
{
    if (autosave_timer) {
        lv_timer_del(autosave_timer);
        autosave_timer = NULL;
    }
    
    if (menu) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
    }
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    text_area = NULL;
    exit_cont = NULL;
    word_count_label = NULL;
    disable_keyboard();
    current_file_path = "";
}

#include "apps/app_registry.h"

namespace {
class TextEditorApp : public core::App {
public:
    TextEditorApp() : core::App("Editor") {}
    void onStart(lv_obj_t *parent) override { ui_text_editor_enter(parent); }
    void onStop() override {
        ui_text_editor_exit(getRoot());
        core::App::onStop();
    }
};
} // namespace

namespace apps {
std::shared_ptr<core::App> make_text_editor_app() {
    return std::make_shared<TextEditorApp>();
}
} // namespace apps
