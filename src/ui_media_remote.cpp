/**
 * @file      ui_media_remote.cpp
 * @brief     BLE HID media remote + keyboard passthrough for iPhone / Android.
 *
 * Phone pairs with the device by its BLE name (see hw_get_ble_kb_name()).
 * Media buttons send HID Consumer Control keys (play/pause, next, prev,
 * vol±) and work against whichever media app is foregrounded on the phone.
 * When the "Keyboard → Phone" switch is on, every keypress on the physical
 * keyboard is also forwarded as a HID keystroke.
 */
#include "ui_define.h"
#include "apps/app_registry.h"

namespace {

static lv_obj_t *status_label = nullptr;
static lv_obj_t *ble_switch = nullptr;
static lv_obj_t *kb_switch = nullptr;
static lv_timer_t *status_timer = nullptr;
static bool last_connected = false;
static bool last_bt_enabled = false;
static bool kb_forward_on = false;

static void forward_key_cb(int state, char &c)
{
    if (state == 1 && kb_forward_on) {
        hw_set_ble_kb_char(&c);
    }
}

static void install_kb_forward(bool on)
{
    kb_forward_on = on;
    hw_set_keyboard_read_callback(on ? forward_key_cb : nullptr);
}

static void refresh_status()
{
    if (!status_label) return;
    bool bt_on = hw_get_bt_enable();
    bool connected = bt_on && hw_get_ble_kb_connected();
    last_bt_enabled = bt_on;
    last_connected = connected;

    if (!bt_on) {
        lv_label_set_text(status_label, LV_SYMBOL_BLUETOOTH "  Off");
        lv_obj_set_style_text_color(status_label, UI_COLOR_MUTED, 0);
    } else if (connected) {
        lv_label_set_text(status_label, LV_SYMBOL_BLUETOOTH "  Connected");
        lv_obj_set_style_text_color(status_label, UI_COLOR_ACCENT, 0);
    } else {
        lv_label_set_text_fmt(status_label,
            LV_SYMBOL_BLUETOOTH "  Pair \"%s\" on phone",
            hw_get_ble_kb_name());
        lv_obj_set_style_text_color(status_label, UI_COLOR_FG, 0);
    }
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    bool bt_on = hw_get_bt_enable();
    bool connected = bt_on && hw_get_ble_kb_connected();
    if (bt_on != last_bt_enabled || connected != last_connected) {
        refresh_status();
    }
}

static void set_toggle_checked(lv_obj_t *btn, bool on)
{
    if (!btn) return;
    if (on) lv_obj_add_state(btn, LV_STATE_CHECKED);
    else    lv_obj_remove_state(btn, LV_STATE_CHECKED);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(btn);
    if (label) lv_label_set_text(label, on ? " On " : " Off ");
}

static void ble_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    hw_feedback();
    hw_set_bt_enable(on);
    if (!on && kb_switch && lv_obj_has_state(kb_switch, LV_STATE_CHECKED)) {
        set_toggle_checked(kb_switch, false);
        install_kb_forward(false);
    }
    refresh_status();
}

static void kb_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    hw_feedback();
    if (on && !hw_get_bt_enable()) {
        hw_set_bt_enable(true);
        set_toggle_checked(ble_switch, true);
    }
    install_kb_forward(on);
    refresh_status();
}

static void send_key_cb(lv_event_t *e)
{
    media_key_value_t key = (media_key_value_t)(intptr_t)lv_event_get_user_data(e);
    hw_feedback();
    hw_set_ble_key(key);
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    menu_show();
}

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_margin_all(p, 0, 0);
    lv_obj_set_style_shadow_width(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *make_key_button(lv_obj_t *parent, const char *symbol,
                                 media_key_value_t key, lv_group_t *grp)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_style_radius(btn, UI_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x151515), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a1a00), LV_STATE_FOCUSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(lbl, lv_theme_get_font_large(lbl), 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, send_key_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)key);
    if (grp) lv_group_add_obj(grp, btn);
    return btn;
}

static void toggle_btn_value_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(btn);
    if (label) {
        lv_label_set_text(label,
            lv_obj_has_state(btn, LV_STATE_CHECKED) ? " On " : " Off ");
    }
}

static lv_obj_t *make_toggle_row(lv_obj_t *parent, const char *label_txt,
                                 bool checked, lv_event_cb_t cb,
                                 lv_group_t *grp)
{
    lv_obj_t *row = make_panel(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 26);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 8, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    lv_obj_set_style_text_color(lbl, UI_COLOR_FG, 0);

    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
    if (checked) lv_obj_add_state(btn, LV_STATE_CHECKED);
    lv_obj_set_width(btn, 54);
    lv_obj_set_height(btn, 22);
    lv_obj_set_style_radius(btn, UI_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x151515), 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);

    lv_obj_t *state_lbl = lv_label_create(btn);
    lv_label_set_text(state_lbl, checked ? " On " : " Off ");
    lv_obj_set_style_text_color(state_lbl, UI_COLOR_FG, 0);
    lv_obj_center(state_lbl);
    lv_obj_set_user_data(btn, state_lbl);

    lv_obj_add_event_cb(btn, toggle_btn_value_cb,
                        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    if (grp) lv_group_add_obj(grp, btn);
    return btn;
}

static lv_obj_t *make_btn_row(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *row = make_panel(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    return row;
}

static void ui_media_remote_enter(lv_obj_t *parent)
{
    ui_show_back_button(back_btn_cb);

    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 4, 0);
    lv_obj_set_style_pad_row(parent, 2, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    status_label = lv_label_create(parent);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(status_label, lv_pct(100));
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_group_t *grp = lv_group_get_default();

    ble_switch = make_toggle_row(parent, "Bluetooth", hw_get_bt_enable(),
                                 ble_switch_cb, grp);

    if (hw_has_keyboard()) {
        kb_switch = make_toggle_row(parent, "Keyboard " LV_SYMBOL_RIGHT " Phone",
                                    false, kb_switch_cb, grp);
    } else {
        kb_switch = nullptr;
    }

    // Transport row: prev / play-pause / next. lv_pct fills remaining space.
    lv_obj_t *row1 = make_btn_row(parent, 44);
    lv_obj_t *first = make_key_button(row1, LV_SYMBOL_PREV,
                                      MEDIA_PREVIOUS, grp);
    make_key_button(row1, LV_SYMBOL_PLAY, MEDIA_PLAY_PAUSE, grp);
    make_key_button(row1, LV_SYMBOL_NEXT, MEDIA_NEXT, grp);

    // Volume row: down / up.
    lv_obj_t *row2 = make_btn_row(parent, 38);
    make_key_button(row2, LV_SYMBOL_VOLUME_MID " " LV_SYMBOL_MINUS,
                    MEDIA_VOLUME_DOWN, grp);
    make_key_button(row2, LV_SYMBOL_VOLUME_MAX " " LV_SYMBOL_PLUS,
                    MEDIA_VOLUME_UP, grp);

    refresh_status();
    if (grp && first) lv_group_focus_obj(first);

    status_timer = lv_timer_create(status_timer_cb, 500, nullptr);
}

static void ui_media_remote_exit(lv_obj_t *parent)
{
    (void)parent;
    ui_hide_back_button();
    if (status_timer) {
        lv_timer_del(status_timer);
        status_timer = nullptr;
    }
    install_kb_forward(false);
    status_label = nullptr;
    ble_switch = nullptr;
    kb_switch = nullptr;
}

class MediaRemoteApp : public core::App {
public:
    MediaRemoteApp() : core::App("Media Remote") {}
    void onStart(lv_obj_t *parent) override {
        setRoot(parent);
        ui_media_remote_enter(parent);
    }
    void onStop() override {
        ui_media_remote_exit(getRoot());
        core::App::onStop();
    }
};

} // namespace

namespace apps {
std::shared_ptr<core::App> make_media_remote_app() {
    return std::make_shared<MediaRemoteApp>();
}
} // namespace apps
