/**
 * @file      ui_lock.cpp
 * @brief     Modal passphrase prompt for the notes encryption feature.
 *
 * Provides an async overlay with a password-mode textarea and OK/Cancel
 * buttons. Callers supply a callback; the prompt fires it with either the
 * typed passphrase (or true/false for the verify-unlock variant) once the
 * user dismisses the modal. Only one prompt at a time.
 */
#include "ui_define.h"
#include "hal/notes_crypto.h"

#include <cstring>
#include <string>

namespace {

struct LockCtx {
    lv_obj_t *overlay      = nullptr;
    lv_obj_t *ta1          = nullptr;
    lv_obj_t *ta2          = nullptr;
    lv_obj_t *err_label    = nullptr;
    lv_obj_t *ok_btn       = nullptr;
    lv_obj_t *cancel_btn   = nullptr;
    lv_group_t *group      = nullptr;
    lv_group_t *prev_group = nullptr;
    bool confirm           = false;
    bool verify_unlock     = false;
    bool password          = true;
    bool allow_empty       = false;
    ui_passphrase_result_cb cb_pw     = nullptr;
    ui_passphrase_unlock_cb cb_unlock = nullptr;
    void *ud               = nullptr;
};

/* Single-instance guard — the caller is expected to wait for one modal
 * before opening another. */
static LockCtx *g_ctx = nullptr;

static void set_error(LockCtx *ctx, const char *msg)
{
    if (!ctx || !ctx->err_label) return;
    lv_label_set_text(ctx->err_label, msg ? msg : "");
    if (msg && *msg) {
        lv_obj_remove_flag(ctx->err_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ctx->err_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void tear_down(LockCtx *ctx)
{
    if (g_ctx != ctx) return;
    if (ctx->overlay) lv_obj_del(ctx->overlay);
    if (ctx->group)   lv_group_del(ctx->group);
    if (ctx->prev_group) set_default_group(ctx->prev_group);
    g_ctx = nullptr;
    delete ctx;
}

static void cancel_event_cb(lv_event_t *e)
{
    LockCtx *ctx = (LockCtx *)lv_event_get_user_data(e);
    if (!ctx) return;
    ui_passphrase_result_cb cb_pw  = ctx->cb_pw;
    ui_passphrase_unlock_cb cb_un  = ctx->cb_unlock;
    void *ud = ctx->ud;
    tear_down(ctx);
    if (cb_pw) cb_pw(nullptr, ud);
    if (cb_un) cb_un(false, ud);
}

static void ok_event_cb(lv_event_t *e)
{
    LockCtx *ctx = (LockCtx *)lv_event_get_user_data(e);
    if (!ctx) return;

    const char *p1 = lv_textarea_get_text(ctx->ta1);
    if ((!p1 || !*p1) && !ctx->allow_empty) {
        set_error(ctx, ctx->password ? "Passphrase can't be empty."
                                     : "Value can't be empty.");
        return;
    }
    if (!p1) p1 = "";
    if (ctx->confirm) {
        const char *p2 = ctx->ta2 ? lv_textarea_get_text(ctx->ta2) : "";
        if (!p2 || strcmp(p1, p2) != 0) {
            set_error(ctx, "Passphrases don't match.");
            return;
        }
    }

    if (ctx->verify_unlock) {
        if (!notes_crypto_unlock(p1)) {
            set_error(ctx, "Wrong passphrase.");
            /* Clear the field so the user can retype rather than hand-delete. */
            lv_textarea_set_text(ctx->ta1, "");
            return;
        }
        ui_passphrase_unlock_cb cb = ctx->cb_unlock;
        void *ud = ctx->ud;
        tear_down(ctx);
        if (cb) cb(true, ud);
        return;
    }

    /* Copy the passphrase before tearing down — the textarea memory is gone
     * after lv_obj_del. */
    std::string pw(p1);
    ui_passphrase_result_cb cb = ctx->cb_pw;
    void *ud = ctx->ud;
    tear_down(ctx);
    if (cb) cb(pw.c_str(), ud);
}

/* Physical-keyboard Enter on a focused textarea submits the form rather
 * than inserting a newline. Encoder ENTER keeps its default (toggle edit). */
static void ta_key_event_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_ENTER) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) return;
    lv_event_stop_processing(e);
    ok_event_cb(e);
}

static void build(const char *title, const char *subtitle,
                  bool confirm, bool verify_unlock,
                  ui_passphrase_result_cb cb_pw,
                  ui_passphrase_unlock_cb cb_unlock,
                  void *ud,
                  bool password = true,
                  const char *initial = nullptr,
                  bool allow_empty = false)
{
    if (g_ctx) {
        /* Already showing a prompt; drop the new request. */
        if (cb_pw)     cb_pw(nullptr, ud);
        if (cb_unlock) cb_unlock(false, ud);
        return;
    }

    LockCtx *ctx = new LockCtx();
    g_ctx = ctx;
    ctx->confirm       = confirm;
    ctx->verify_unlock = verify_unlock;
    ctx->password      = password;
    ctx->allow_empty   = allow_empty;
    ctx->cb_pw         = cb_pw;
    ctx->cb_unlock     = cb_unlock;
    ctx->ud            = ud;

    ctx->prev_group = lv_group_get_default();
    ctx->group = lv_group_create();
    lv_group_set_wrap(ctx->group, true);
    set_default_group(ctx->group);

    enable_keyboard();

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    ctx->overlay = overlay;
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(overlay);
    lv_obj_set_style_bg_color(overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 10, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(overlay, 6, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    if (title) {
        lv_obj_t *t = lv_label_create(overlay);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(t, lv_theme_get_font_large(t), 0);
    }
    if (subtitle) {
        lv_obj_t *s = lv_label_create(overlay);
        lv_label_set_text(s, subtitle);
        lv_obj_set_style_text_color(s, UI_COLOR_MUTED, 0);
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s, lv_pct(90));
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    }

    auto style_ta = [](lv_obj_t *ta) {
        lv_obj_set_style_bg_color(ta, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(ta, UI_COLOR_FG, 0);
        lv_obj_set_style_border_color(ta, UI_COLOR_MUTED, 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_border_color(ta, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(ta, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUS_KEY);
    };

    ctx->ta1 = lv_textarea_create(overlay);
    lv_textarea_set_one_line(ctx->ta1, true);
    lv_textarea_set_password_mode(ctx->ta1, password);
    if (initial && *initial) lv_textarea_set_text(ctx->ta1, initial);
    lv_obj_set_width(ctx->ta1, lv_pct(90));
    style_ta(ctx->ta1);
    lv_group_add_obj(ctx->group, ctx->ta1);
    lv_obj_add_event_cb(ctx->ta1, ta_key_event_cb, LV_EVENT_KEY, ctx);

    if (confirm) {
        ctx->ta2 = lv_textarea_create(overlay);
        lv_textarea_set_one_line(ctx->ta2, true);
        lv_textarea_set_password_mode(ctx->ta2, true);
        lv_textarea_set_placeholder_text(ctx->ta2, "confirm");
        lv_obj_set_width(ctx->ta2, lv_pct(90));
        style_ta(ctx->ta2);
        lv_group_add_obj(ctx->group, ctx->ta2);
        lv_obj_add_event_cb(ctx->ta2, ta_key_event_cb, LV_EVENT_KEY, ctx);
    }

    ctx->err_label = lv_label_create(overlay);
    lv_label_set_text(ctx->err_label, "");
    lv_obj_set_style_text_color(ctx->err_label, lv_palette_main(LV_PALETTE_RED), 0);
    lv_label_set_long_mode(ctx->err_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->err_label, lv_pct(90));
    lv_obj_set_style_text_align(ctx->err_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(ctx->err_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *btn_row = lv_obj_create(overlay);
    lv_obj_set_size(btn_row, lv_pct(95), 44);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, 6, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Matches the remote screen's button styling (see make_key_button in
     * ui_media_remote.cpp). */
    auto style_btn = [](lv_obj_t *btn) {
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, lv_pct(100));
        lv_obj_set_style_radius(btn, UI_RADIUS, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x151515), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_FG, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_border_width(btn, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a1a00), LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
    };

    ctx->cancel_btn = lv_btn_create(btn_row);
    style_btn(ctx->cancel_btn);
    {
        lv_obj_t *lbl = lv_label_create(ctx->cancel_btn);
        lv_label_set_text(lbl, "Cancel");
        lv_obj_set_style_text_color(lbl, UI_COLOR_FG, 0);
        lv_obj_center(lbl);
    }
    lv_obj_add_event_cb(ctx->cancel_btn, cancel_event_cb, LV_EVENT_CLICKED, ctx);
    lv_group_add_obj(ctx->group, ctx->cancel_btn);

    ctx->ok_btn = lv_btn_create(btn_row);
    style_btn(ctx->ok_btn);
    {
        lv_obj_t *lbl = lv_label_create(ctx->ok_btn);
        lv_label_set_text(lbl, "OK");
        lv_obj_set_style_text_color(lbl, UI_COLOR_FG, 0);
        lv_obj_center(lbl);
    }
    lv_obj_add_event_cb(ctx->ok_btn, ok_event_cb, LV_EVENT_CLICKED, ctx);
    lv_group_add_obj(ctx->group, ctx->ok_btn);

    lv_group_focus_obj(ctx->ta1);
}

} /* namespace */

/* Public API (see ui_define.h). */

void ui_passphrase_unlock(ui_passphrase_unlock_cb cb, void *ud)
{
    if (!notes_crypto_is_enabled() || notes_crypto_is_unlocked()) {
        if (cb) cb(true, ud);
        return;
    }
    build("Unlock Notes",
          "Enter your passphrase",
          /*confirm=*/false, /*verify_unlock=*/true,
          /*cb_pw=*/nullptr, cb, ud);
}

void ui_passphrase_prompt(const char *title, const char *subtitle,
                          bool confirm,
                          ui_passphrase_result_cb cb, void *ud)
{
    build(title, subtitle, confirm, /*verify_unlock=*/false,
          cb, nullptr, ud);
}

void ui_text_prompt(const char *title, const char *subtitle,
                    const char *initial,
                    ui_passphrase_result_cb cb, void *ud)
{
    build(title, subtitle, /*confirm=*/false, /*verify_unlock=*/false,
          cb, nullptr, ud,
          /*password=*/false, initial, /*allow_empty=*/true);
}
