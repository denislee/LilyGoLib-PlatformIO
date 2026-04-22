/**
 * @file      ui_list_picker.cpp
 * @brief     Modal list-picker overlay.
 *
 * Mirrors the look/feel of ui_wifi.cpp's overlay: sits on lv_layer_top below
 * the status bar, takes over the back button to close, and focuses the first
 * item so keyboard/encoder navigation Just Works. Only one picker can be
 * open at a time — subsequent opens are ignored while one is live.
 */
#include "ui_list_picker.h"
#include "ui_define.h"

namespace {

struct PickerCtx {
    lv_obj_t   *overlay    = nullptr;
    lv_obj_t   *list       = nullptr;
    lv_group_t *group      = nullptr;
    lv_group_t *prev_group = nullptr;
    lv_event_cb_t prev_back_cb = nullptr;
    ui_list_picker_cb cb = nullptr;
    void *ud = nullptr;
    bool fired = false;
};

static PickerCtx *g_ctx = nullptr;

static void tear_down(int picked_index)
{
    if (!g_ctx) return;
    PickerCtx *ctx = g_ctx;
    g_ctx = nullptr;

    // Fire the callback BEFORE destroying LVGL objects so the caller can
    // reopen the picker or open another modal without racing our teardown.
    ui_list_picker_cb cb = ctx->cb;
    void *ud = ctx->ud;
    bool will_fire = !ctx->fired && cb;
    ctx->fired = true;

    if (ctx->overlay) lv_obj_del(ctx->overlay);
    if (ctx->group)   lv_group_del(ctx->group);
    if (ctx->prev_group) set_default_group(ctx->prev_group);
    if (ctx->prev_back_cb) ui_show_back_button(ctx->prev_back_cb);
    delete ctx;

    if (will_fire) cb(picked_index, ud);
}

static void close_cb(lv_event_t *) { tear_down(-1); }

static void item_clicked_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    if (!btn || !g_ctx) return;
    intptr_t idx = (intptr_t)lv_obj_get_user_data(btn);
    tear_down((int)idx);
}

} // namespace

void ui_list_picker_open(const char *title,
                         const std::vector<std::string> &items,
                         ui_list_picker_cb cb, void *ud)
{
    if (g_ctx) {
        // A picker is already up. Don't stack; fire cancel for the caller.
        if (cb) cb(-1, ud);
        return;
    }

    PickerCtx *ctx = new PickerCtx();
    g_ctx = ctx;
    ctx->cb = cb;
    ctx->ud = ud;

    ctx->prev_group = lv_group_get_default();
    ctx->prev_back_cb = ui_get_back_button_cb();
    ctx->group = lv_group_create();
    lv_group_set_wrap(ctx->group, false);
    set_default_group(ctx->group);

    ui_show_back_button(close_cb);
    enable_keyboard();

    // Match the status-bar height, same math as ui_wifi.
    const lv_font_t *header_font = get_header_font();
    int32_t bar_h = lv_font_get_line_height(header_font) + 8;
    if (bar_h < 30) bar_h = 30;
    int32_t v_res = lv_display_get_vertical_resolution(NULL);
    if (v_res <= 0) v_res = 222;

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    ctx->overlay = overlay;
    lv_obj_set_size(overlay, lv_pct(100), v_res - bar_h);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 8, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(overlay, 6, 0);

    if (title && *title) {
        lv_obj_t *t = lv_label_create(overlay);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(t, lv_pct(100));
    }

    ctx->list = lv_list_create(overlay);
    lv_obj_set_width(ctx->list, lv_pct(100));
    lv_obj_set_flex_grow(ctx->list, 1);
    lv_obj_set_style_radius(ctx->list, UI_RADIUS, 0);

    if (items.empty()) {
        lv_obj_t *empty = lv_label_create(ctx->list);
        lv_label_set_text(empty, "(no results)");
        lv_obj_set_style_text_color(empty, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_pad_all(empty, 16, 0);
        return;
    }

    lv_obj_t *first = nullptr;
    for (size_t i = 0; i < items.size(); ++i) {
        lv_obj_t *btn = lv_list_add_btn(ctx->list, LV_SYMBOL_RIGHT, items[i].c_str());
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, item_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_group_add_obj(ctx->group, btn);
        if (!first) first = btn;
    }
    if (first) lv_group_focus_obj(first);
}
