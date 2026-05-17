/**
 * @file      menu_glance.cpp
 * @brief     Glance overlay implementation — see menu_glance.h.
 *
 * Extracted from menu_app.cpp. Owns its own lv_layer_top overlay,
 * dedicated lv_group_t so input rolls over the cards, and a 1 Hz timer
 * that re-formats clock/battery/connectivity/unread labels.
 */
#include "menu_glance.h"

#include "../ui_define.h"
#include "../hal/wireless.h"
#include "../hal/hub.h"

#include <cstring>

LV_FONT_DECLARE(lv_font_montserrat_18);

namespace apps {

/* tg_get_unread_count() is defined in ui_telegram.cpp. Forward-decl to
 * avoid pulling that 1.5k-line TU's includes here. */
int tg_get_unread_count();

namespace menu {
namespace {

static lv_obj_t  *s_glance_overlay = nullptr;
static lv_group_t *s_glance_group  = nullptr;
static lv_group_t *s_glance_prev_group = nullptr;
static lv_obj_t  *s_glance_time_lbl = nullptr;
static lv_obj_t  *s_glance_date_lbl = nullptr;
static lv_obj_t  *s_glance_batt_lbl = nullptr;
static lv_obj_t  *s_glance_conn_lbl = nullptr;
static lv_obj_t  *s_glance_tg_lbl = nullptr;     // Telegram unread count
static lv_obj_t  *s_glance_tg_card = nullptr;    // wrapper, kept visible at 0 unread
static lv_timer_t *s_glance_timer = nullptr;

static void glance_refresh(lv_timer_t *t) {
    (void)t;
    if (!s_glance_overlay) return;

    struct tm timeinfo;
    hw_get_date_time(timeinfo);
    if (s_glance_time_lbl) {
        lv_label_set_text_fmt(s_glance_time_lbl, "%02d:%02d",
                              timeinfo.tm_hour, timeinfo.tm_min);
    }
    if (s_glance_date_lbl) {
        static const char *kDays[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char *kMonths[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec"};
        int wd = timeinfo.tm_wday; if (wd < 0 || wd > 6) wd = 0;
        int mo = timeinfo.tm_mon;  if (mo < 0 || mo > 11) mo = 0;
        lv_label_set_text_fmt(s_glance_date_lbl, "%s, %s %d %d",
                              kDays[wd], kMonths[mo], timeinfo.tm_mday,
                              timeinfo.tm_year + 1900);
    }
    if (s_glance_batt_lbl) {
        monitor_params_t params;
        hw_get_monitor_params(params);
        const char *sym = LV_SYMBOL_BATTERY_FULL;
        if (params.is_charging) sym = LV_SYMBOL_CHARGE;
        else if (params.battery_percent < 20) sym = LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_glance_batt_lbl, "%s  %d%%",
                              sym, params.battery_percent);
    }
    if (s_glance_conn_lbl) {
        // Single line of active connectivity glyphs so the user can tell
        // whether radios are up without leaving the glance.
        char buf[64];
        size_t off = 0;
        auto append = [&](const char *s) {
            if (!s) return;
            size_t l = strlen(s);
            if (off + l + 2 >= sizeof(buf)) return;
            if (off > 0) { buf[off++] = ' '; buf[off++] = ' '; }
            memcpy(buf + off, s, l);
            off += l;
        };
        if (hw_get_wifi_connected())     append(LV_SYMBOL_WIFI);
        if (hw_get_bt_enable() && hw_get_ble_kb_connected())
                                         append(LV_SYMBOL_BLUETOOTH);
        if (hal::hub_is_enabled())       append(LV_SYMBOL_HOME);
        buf[off] = 0;
        lv_label_set_text(s_glance_conn_lbl, buf);
    }
    if (s_glance_tg_lbl) {
        int unread = apps::tg_get_unread_count();
        lv_label_set_text_fmt(s_glance_tg_lbl,
                              LV_SYMBOL_ENVELOPE "  %d", unread);
        lv_obj_set_style_text_color(s_glance_tg_lbl,
                                    unread > 0 ? UI_COLOR_FG : UI_COLOR_MUTED, 0);
    }
}

static void glance_dismiss();

static void glance_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    // Any tap (on release) or any key dismisses. Skipping LV_EVENT_PRESSED
    // avoids leaking the click to the menu underneath on release.
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_KEY) {
        glance_dismiss();
    }
}

static void glance_dismiss() {
    if (!s_glance_overlay) return;
    if (s_glance_timer) {
        lv_timer_del(s_glance_timer);
        s_glance_timer = nullptr;
    }
    lv_obj_del(s_glance_overlay);
    s_glance_overlay = nullptr;
    s_glance_tg_lbl = nullptr;
    s_glance_tg_card = nullptr;
    s_glance_time_lbl = nullptr;
    s_glance_date_lbl = nullptr;
    s_glance_batt_lbl = nullptr;
    s_glance_conn_lbl = nullptr;

    // Restore the menu's input group so encoder/keyboard nav resumes.
    if (s_glance_prev_group) {
        set_default_group(s_glance_prev_group);
        s_glance_prev_group = nullptr;
    }
    if (s_glance_group) {
        lv_group_del(s_glance_group);
        s_glance_group = nullptr;
    }
}

// Build a bento "card" container — rounded dark panel with centred flex
// content. Used for every cell in the glance grid so the visual language is
// uniform regardless of which payload sits inside.
static lv_obj_t *glance_make_card(lv_obj_t *parent) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_pad_row(c, 2, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return c;
}

// Transparent flex wrapper — used for row containers inside the bento so
// cards line up without an extra panel showing through. Caller sets the
// row's flex_grow share of the parent height; cards inside the row use
// height lv_pct(100) so they all match.
static lv_obj_t *glance_make_row(lv_obj_t *parent, lv_flex_flow_t flow) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, 0);  // height comes from flex_grow set by caller
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_row(r, 6, 0);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, flow);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

} // namespace

void glance_show() {
    if (s_glance_overlay) return;

    s_glance_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_glance_overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(s_glance_overlay);
    lv_obj_set_style_bg_color(s_glance_overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_glance_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_glance_overlay, 0, 0);
    lv_obj_set_style_radius(s_glance_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_glance_overlay, 0, 0);
    lv_obj_remove_flag(s_glance_overlay, LV_OBJ_FLAG_SCROLLABLE);
    // The overlay needs to be clickable so a tap on the panel dismisses it.
    lv_obj_add_flag(s_glance_overlay, LV_OBJ_FLAG_CLICKABLE);

    // Bento content container. The whole stack is rotated 180° around its
    // centre so a user holding the device upside-down reads it right-way-up.
    // With LV_FLEX_FLOW_COLUMN_REVERSE + LV_FLEX_ALIGN_START, the first child
    // pins to the bottom of the unrotated layout — the *top* of the screen
    // after the 180° rotation. Children are added top-to-bottom: status row,
    // info row, then time hero last so the clock dominates the bottom.
    lv_obj_t *content = lv_obj_create(s_glance_overlay);
    lv_obj_set_size(content, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN_REVERSE);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_transform_pivot_x(content, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(content, lv_pct(50), 0);
    lv_obj_set_style_transform_rotation(content, 1800, 0);

    // Row 1 (visual top) — battery | connectivity, side-by-side. ROW_REVERSE
    // so that post-rotation the battery card reads on the left, conn on right.
    lv_obj_t *row_status = glance_make_row(content, LV_FLEX_FLOW_ROW_REVERSE);
    lv_obj_set_flex_grow(row_status, 1);

    lv_obj_t *batt_card = glance_make_card(row_status);
    lv_obj_set_height(batt_card, lv_pct(100));
    lv_obj_set_flex_grow(batt_card, 1);
    s_glance_batt_lbl = lv_label_create(batt_card);
    lv_obj_set_style_text_color(s_glance_batt_lbl, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_glance_batt_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_glance_batt_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *conn_card = glance_make_card(row_status);
    lv_obj_set_height(conn_card, lv_pct(100));
    lv_obj_set_flex_grow(conn_card, 1);
    s_glance_conn_lbl = lv_label_create(conn_card);
    lv_obj_set_style_text_color(s_glance_conn_lbl, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_glance_conn_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_glance_conn_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // Row 2 — date | telegram unread. Both cards always visible so the row
    // stays a balanced 50/50; the TG card just dims at 0 unread.
    lv_obj_t *row_info = glance_make_row(content, LV_FLEX_FLOW_ROW_REVERSE);
    lv_obj_set_flex_grow(row_info, 1);

    lv_obj_t *date_card = glance_make_card(row_info);
    lv_obj_set_height(date_card, lv_pct(100));
    lv_obj_set_flex_grow(date_card, 1);
    s_glance_date_lbl = lv_label_create(date_card);
    lv_obj_set_style_text_color(s_glance_date_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_glance_date_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_glance_date_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_glance_tg_card = glance_make_card(row_info);
    lv_obj_set_height(s_glance_tg_card, lv_pct(100));
    lv_obj_set_flex_grow(s_glance_tg_card, 1);
    s_glance_tg_lbl = lv_label_create(s_glance_tg_card);
    lv_obj_set_style_text_color(s_glance_tg_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_glance_tg_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_glance_tg_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // Row 3 (visual bottom) — TIME hero card. Wider grow share so the clock
    // dominates the screen vertically.
    lv_obj_t *time_card = glance_make_card(content);
    lv_obj_set_width(time_card, lv_pct(100));
    lv_obj_set_height(time_card, 0);
    lv_obj_set_flex_grow(time_card, 2);
    s_glance_time_lbl = lv_label_create(time_card);
    lv_obj_set_style_text_color(s_glance_time_lbl, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_glance_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(s_glance_time_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // Capture every input on the overlay so any key/tap dismisses. The
    // overlay is the only object in a private group while open, so encoder
    // rotation events also land here and trigger LV_EVENT_KEY.
    lv_obj_add_event_cb(s_glance_overlay, glance_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_glance_overlay, glance_event_cb, LV_EVENT_KEY, nullptr);

    s_glance_prev_group = lv_group_get_default();
    s_glance_group = lv_group_create();
    lv_group_set_wrap(s_glance_group, false);
    lv_group_add_obj(s_glance_group, s_glance_overlay);
    set_default_group(s_glance_group);
    lv_group_focus_obj(s_glance_overlay);

    glance_refresh(nullptr);
    s_glance_timer = lv_timer_create(glance_refresh, 1000, nullptr);
}

} // namespace menu
} // namespace apps
