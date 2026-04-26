/**
 * @file      ui_define.h
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#ifdef ARDUINO
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_mac.h>
#else
#define RTC_DATA_ATTR
#endif
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include "hal_interface.h"
#include "core/system_hooks.h"

#define DEFAULT_OPA          100

/* Unified theme tokens. Use these everywhere instead of hard-coded colors. */
#define UI_COLOR_ACCENT      lv_palette_main(LV_PALETTE_ORANGE)
#define UI_COLOR_MUTED       lv_palette_main(LV_PALETTE_GREY)
#define UI_COLOR_BG          lv_color_black()
#define UI_COLOR_FG          lv_color_white()
#define UI_RADIUS            8
#define UI_BORDER_W          2

enum {
    LV_MENU_ITEM_BUILDER_VARIANT_1,
    LV_MENU_ITEM_BUILDER_VARIANT_2
};
typedef uint8_t lv_menu_builder_variant_t;

#define MSG_MENU_NAME_CHANGED    100
#define MSG_LABEL_PARAM_CHANGE_1 200
#define MSG_LABEL_PARAM_CHANGE_2 201
#define MSG_TITLE_NAME_CHANGE    203
#define MSG_BLE_SEND_DATA_1      204
#define MSG_BLE_SEND_DATA_2      205
#define MSG_MUSIC_TIME_ID        300
#define MSG_MUSIC_TIME_END_ID    301
#define MSG_FFT_ID               400

extern lv_obj_t *main_screen;
extern lv_obj_t *menu_panel;
extern lv_obj_t *app_panel;
extern lv_group_t *menu_g;
extern lv_group_t *app_g;

lv_obj_t *ui_create_option(lv_obj_t *parent, const char *title, const char *symbol_txt, lv_obj_t *(*widget_create)(lv_obj_t *parent), lv_event_cb_t btn_event_cb);
lv_obj_t *create_text(lv_obj_t *parent, const char *icon, const char *txt,
                      lv_menu_builder_variant_t builder_variant);
lv_obj_t *create_slider(lv_obj_t *parent, const char *icon, const char *txt, int32_t min, int32_t max,
                        int32_t val, lv_event_cb_t cb, lv_event_code_t filter);
lv_obj_t *create_switch(lv_obj_t *parent, const char *icon, const char *txt, bool chk, lv_event_cb_t cb);
lv_obj_t *create_button(lv_obj_t *parent, const char *icon, const char *txt, lv_event_cb_t cb);
lv_obj_t *create_label(lv_obj_t *parent, const char *icon, const char *txt, const char *default_text);
lv_obj_t *create_dropdown(lv_obj_t *parent, const char *icon, const char *txt, const char *options, uint8_t default_sel, lv_event_cb_t cb);
lv_obj_t *create_msgbox(lv_obj_t *parent, const char *title_txt,
                        const char *msg_txt, const char **btns,
                        lv_event_cb_t btns_event_cb, void *user_data);
void destroy_msgbox(lv_obj_t *msgbox);

lv_indev_t *lv_get_encoder_indev();
lv_indev_t *lv_get_keyboard_indev();
/* menu_show / menu_hidden declared in core/system_hooks.h (via include above). */
void set_default_group(lv_group_t *group);

lv_obj_t *ui_create_process_bar(lv_obj_t *parent, const char *title);

/* Themed fullscreen modal overlay. Caller appends children (labels, bar, buttons)
 * to the returned object; destroy with ui_popup_destroy(). Used for loading
 * overlays, progress dialogs, and other short-lived modals that need the same
 * look as create_msgbox() (black bg, accent border, white text). */
lv_obj_t *ui_popup_create(const char *title);
void ui_popup_destroy(lv_obj_t *popup);

/* Unified loading / progress popup. Every long-running screen (sync,
 * download, index scan, storage ops) should use this instead of hand-
 * building lv_obj_t trees atop ui_popup_create() — produces a consistent
 * look: title at top, spinner OR progress bar centered, counts + detail
 * lines below.
 *
 * Typical usage:
 *   ui_loading_t ld;
 *   ui_loading_open(&ld, "Syncing news", "Connecting...");
 *   ui_loading_set_progress(&ld, i, total, fname);
 *   ...
 *   ui_loading_close(&ld);
 *
 * After open() the popup is indeterminate (spinner + detail line). The
 * first set_progress() call swaps the spinner for a real progress bar
 * and shows "cur / total (pct%)" counts. set_indeterminate() swaps back. */
typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *spinner;
    lv_obj_t *bar;
    lv_obj_t *counts;
    lv_obj_t *detail;
} ui_loading_t;

void ui_loading_open(ui_loading_t *l, const char *title, const char *detail);
void ui_loading_set_indeterminate(ui_loading_t *l, const char *detail);
void ui_loading_set_progress(ui_loading_t *l, int cur, int total, const char *detail);
void ui_loading_close(ui_loading_t *l);

/* Structured result popup. Same two-button modal as ui_msg_pop_up, but the
 * body is rendered as aligned "label: value" rows so numeric summaries
 * (files synced / failed / skipped) read as a table instead of a comma-
 * separated sentence. Pass nullptr/0 rows to fall back to a plain subtitle
 * body.
 *
 * Example:
 *   ui_summary_row_t rows[] = {
 *       {"Synced",  "3 / 10"},
 *       {"Failed",  "2"},
 *       {"Skipped", "5"},
 *   };
 *   ui_result_show("News sync", "Done.", rows, 3); */
typedef struct {
    const char *label;
    const char *value;
} ui_summary_row_t;

void ui_result_show(const char *title, const char *subtitle,
                    const ui_summary_row_t *rows, size_t n_rows);

void theme_init();

void disable_input_devices();
void enable_input_devices();

void disable_keyboard();
void enable_keyboard();

void ui_text_editor_open_file(const char *path);
void ui_text_editor_new_document();

/* Notes-encryption passphrase prompts (see ui_lock.cpp). */
typedef void (*ui_passphrase_result_cb)(const char *pw, void *ud);
typedef void (*ui_passphrase_unlock_cb)(bool ok, void *ud);

/* Runs the session-unlock flow. Fires `cb(true, ud)` immediately if crypto
 * is disabled or already unlocked; otherwise shows the passphrase modal
 * and calls `cb` with the outcome. */
void ui_passphrase_unlock(ui_passphrase_unlock_cb cb, void *ud);

/* Low-level prompt. `cb` receives the typed passphrase (non-null, only valid
 * during the callback) on OK, or NULL on Cancel. With `confirm=true`, a second
 * confirmation field is shown and only matching entries proceed. */
void ui_passphrase_prompt(const char *title, const char *subtitle,
                          bool confirm,
                          ui_passphrase_result_cb cb, void *ud);

/* Plain-text modal prompt. Same look as ui_passphrase_prompt but with no
 * password masking. `initial` pre-fills the field (NULL = empty). `cb` fires
 * with the typed text on OK (only valid during the callback) or NULL on
 * Cancel. */
void ui_text_prompt(const char *title, const char *subtitle,
                    const char *initial,
                    ui_passphrase_result_cb cb, void *ud);

/* Device-level lock enforcement. If notes crypto is enabled and the session
 * is locked, puts the unlock modal on top of the UI and keeps it there — any
 * cancel immediately re-opens the modal — until the correct passphrase is
 * entered. No-op when crypto is disabled or the session is already unlocked. */
void ui_device_lock_enforce();

/* Modal WiFi picker: scan / list / connect / forget. Self-contained overlay
 * on lv_layer_top. See ui_wifi.cpp. */
void ui_wifi_networks_open();
/* Live NFC status + detection counters overlay. See ui_nfc_test.cpp. */
void ui_nfc_test_open();
/* Editor / fake-sleep / instance-lock hooks are declared in
 * core/system_hooks.h (included at top of this file). */

const lv_font_t *get_editor_font();
const lv_font_t *get_small_font();
const lv_font_t *get_journal_font();
const lv_font_t *get_md_font();
const lv_font_t *get_header_font();
const lv_font_t *get_home_font();
const lv_font_t *get_system_font();
const lv_font_t *get_weather_font();
const lv_font_t *get_telegram_font();
const lv_font_t *get_telegram_list_font();
const lv_font_t *get_ssh_font();

lv_obj_t *create_floating_button(lv_event_cb_t event_cb, void* user_data);
lv_obj_t *create_menu(lv_obj_t *parent, lv_event_cb_t event_cb);
lv_obj_t *create_radius_button(lv_obj_t *parent, const void *image, lv_event_cb_t event_cb, void* user_data);
lv_obj_t *create_back_button(lv_obj_t *parent, lv_event_cb_t cb);

/* Show the shared back button on the top status bar (where clock/battery live).
 * Replaces any previous callback and adds the button to the current default
 * lv_group so it's keyboard-navigable. Returns the button object so callers
 * can attach additional event handlers or use it as a focus target. */
lv_obj_t *ui_show_back_button(lv_event_cb_t cb);
void ui_hide_back_button(void);
lv_event_cb_t ui_get_back_button_cb(void);

#if LVGL_VERSION_MAJOR == 9
#define LV_MENU_ROOT_BACK_BTN_ENABLED   LV_MENU_ROOT_BACK_BUTTON_ENABLED
#define lv_menu_back_btn_is_root        lv_menu_back_button_is_root
#define lv_menu_set_mode_root_back_btn  lv_menu_set_mode_root_back_button
#define lv_mem_alloc                    lv_malloc
#define lv_mem_free                     lv_free
#define LV_IMG_CF_ALPHA_8BIT            LV_COLOR_FORMAT_L8
#define lv_point_t                      lv_point_precise_t
#else
#define lv_timer_get_user_data(x)       (x->user_data)
#define lv_indev_get_type(x)            (x->driver->type)
#endif

#if LVGL_VERSION_MAJOR == 8

#endif

#ifndef M_PI
#define M_PI		3.14159265358979323846
#endif
