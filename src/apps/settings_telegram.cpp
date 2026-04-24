/**
 * @file      settings_telegram.cpp
 * @brief     Settings » Telegram + » Favorites subpages. Extracted from
 *            ui_settings.cpp; see settings_internal.h for the cross-TU
 *            contract.
 *
 * The actual storage lives in ui_telegram.cpp (NVS + optional AES wrap via
 * notes crypto). This file is just the UI over `apps::tg_cfg_*`.
 */
#include "../ui_define.h"
#include "../hal/notes_crypto.h"
#include "app_registry.h"
#include "settings_internal.h"

namespace telegram_cfg {

static lv_obj_t *g_sub_page       = nullptr;
static lv_obj_t *g_url_label      = nullptr;
static lv_obj_t *g_tok_label      = nullptr;
static lv_obj_t *g_note_label     = nullptr;
static lv_obj_t *g_fav_sub_page   = nullptr;

// Kept for the Favorites subpage — needs the parent menu object to call
// lv_menu_set_page from the button callback.
static lv_obj_t *g_parent_menu    = nullptr;

void set_sub_page(lv_obj_t *page) { g_sub_page = page; }

void reset_state()
{
    g_sub_page = nullptr;
    g_url_label = nullptr;
    g_tok_label = nullptr;
    g_note_label = nullptr;
    g_fav_sub_page = nullptr;
    g_parent_menu = nullptr;
}

static void build_favorites_subpage(lv_obj_t *menu, lv_obj_t *sub_page);

static void refresh_labels()
{
    if (g_url_label) {
        std::string url = apps::tg_cfg_get_url();
        lv_label_set_text_fmt(g_url_label, "URL: %s",
                              url.empty() ? "(not set)" : url.c_str());
    }
    if (g_tok_label) {
        std::string tok = apps::tg_cfg_get_token_display();
        const char *suffix = apps::tg_cfg_token_is_encrypted()
                             ? "  [encrypted]" : "  [plaintext]";
        lv_label_set_text_fmt(g_tok_label, "Token: %s%s",
                              tok.c_str(), suffix);
    }
#ifdef ARDUINO
    if (g_note_label) {
        const char *note;
        if (!notes_crypto_is_enabled()) {
            note = "Enable Notes encryption (Settings » Notes Security) "
                   "before saving the bearer — the token is persisted "
                   "only AES-256 wrapped, never plaintext.";
        } else if (!notes_crypto_is_unlocked()) {
            note = "Notes session locked — unlock (open Notes) to "
                   "save or read the token.";
        } else {
            note = "Token is AES-256 wrapped with your notes passphrase.";
        }
        lv_label_set_text(g_note_label, note);
    }
#endif
}

static void set_url_cb(const char *text, void *)
{
    if (!text) return;
    apps::tg_cfg_set_url(text);
    refresh_labels();
}

static void set_token_cb(const char *text, void *)
{
    if (!text) return;
    std::string err;
    if (!apps::tg_cfg_set_token(text, &err)) {
        ui_msg_pop_up("Token rejected",
                      err.empty() ? "Save failed." : err.c_str());
    }
    refresh_labels();
}

static void btn_set_url_cb(lv_event_t *)
{
    std::string current = apps::tg_cfg_get_url();
    ui_text_prompt("Bridge URL", "https://tg.example.com",
                   current.c_str(), set_url_cb, nullptr);
}

static void btn_open_favorites_cb(lv_event_t *)
{
    if (!g_parent_menu || !g_fav_sub_page) return;
    lv_menu_set_page(g_parent_menu, g_fav_sub_page);
}

static void btn_set_token_cb(lv_event_t *)
{
    // Intentionally don't pre-fill the token so we never read plaintext out
    // of NVS just to build a prompt — ui_text_prompt would echo it on-screen.
    ui_text_prompt("Bearer token", "from config.yaml on the bridge",
                   "", set_token_cb, nullptr);
}

// Notification toggles mirror the haptic_enable pattern: the toggle btn
// holds the " On "/" Off " label as its user_data so the callback can flip
// the text in place when the user changes it.
static void notif_vibrate_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    apps::tg_cfg_set_notif_vibrate(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

static void notif_banner_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    apps::tg_cfg_set_notif_banner(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    g_parent_menu = menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(status, 2, 0);

    g_url_label = lv_label_create(status);
    lv_label_set_long_mode(g_url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_url_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_url_label, UI_COLOR_MUTED, 0);

    g_tok_label = lv_label_create(status);
    lv_label_set_long_mode(g_tok_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_tok_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_tok_label, UI_COLOR_MUTED, 0);

    lv_obj_t *b1 = create_button(sub_page, LV_SYMBOL_WIFI,
                                 "Set URL", btn_set_url_cb);
    register_subpage_group_obj(sub_page, b1);

    lv_obj_t *b2 = create_button(sub_page, LV_SYMBOL_KEYBOARD,
                                 "Set token", btn_set_token_cb);
    register_subpage_group_obj(sub_page, b2);

    // Favorites: clicking opens a nested subpage that fetches the full chat
    // list from the bridge and lets the user star/unstar each chat. The
    // Telegram app's listing renders only the starred ones. Match the
    // Set URL / Set token rows exactly — create_button (real cb) gives us the
    // same chevron bubble, and the cb triggers navigation via
    // lv_menu_set_page instead of relying on lv_menu_set_load_page_event
    // (which doesn't play well with a child btn inside the menu_cont).
    g_fav_sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(g_fav_sub_page, (void*)build_favorites_subpage);
    lv_obj_t *b3 = create_button(sub_page, LV_SYMBOL_EYE_OPEN,
                                 "Favorites", btn_open_favorites_cb);
    register_subpage_group_obj(sub_page, b3);

    // Per-channel notification toggles. The background poll (running even
    // when the Telegram app is closed) dispatches to these when it sees the
    // total unread count go up. Haptic respects the global Haptic toggle in
    // Connectivity — if that's off, enabling "Vibrate" here still won't
    // buzz.
    lv_obj_t *b4 = create_toggle_btn_row(sub_page, "Vibrate on new msg",
                                         apps::tg_cfg_get_notif_vibrate(),
                                         notif_vibrate_cb);
    register_subpage_group_obj(sub_page, b4);

    lv_obj_t *b5 = create_toggle_btn_row(sub_page, "Banner on new msg",
                                         apps::tg_cfg_get_notif_banner(),
                                         notif_banner_cb);
    register_subpage_group_obj(sub_page, b5);

#ifdef ARDUINO
    lv_obj_t *note_row = lv_menu_cont_create(sub_page);
    g_note_label = lv_label_create(note_row);
    lv_label_set_long_mode(g_note_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_note_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_note_label, UI_COLOR_MUTED, 0);
#endif

    refresh_labels();
}

// Per-button owned chat id — freed via LV_EVENT_DELETE since `long long`
// won't fit in a 32-bit void* slot on the ESP32-S3.
static void fav_id_delete_cb(lv_event_t *e)
{
    auto *id = (long long *)lv_event_get_user_data(e);
    delete id;
}

static void fav_toggle_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    auto *id = (long long *)lv_event_get_user_data(e);
    if (!id) return;
    bool checked = lv_obj_has_state(btn, LV_STATE_CHECKED);
    apps::tg_cfg_set_favorite(*id, checked);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(btn);
    if (label) lv_label_set_text(label, checked ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
}

static void build_favorites_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    lv_obj_t *status_row = lv_menu_cont_create(sub_page);
    lv_obj_t *status_lbl = lv_label_create(status_row);
    lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(status_lbl, UI_COLOR_MUTED, 0);
    lv_label_set_text(status_lbl, "Loading chats...");
    lv_refr_now(NULL);

    std::vector<std::pair<long long, std::string>> chats;
    std::string err;
    bool ok = apps::tg_cfg_fetch_all_chats(chats, &err);
    if (!ok) {
        lv_label_set_text_fmt(status_lbl, "Fetch failed: %s", err.c_str());
        return;
    }
    if (chats.empty()) {
        lv_label_set_text(status_lbl, "Bridge returned no chats.");
        return;
    }
    lv_label_set_text_fmt(status_lbl,
        "%u chat(s) — tap to toggle favorite.", (unsigned)chats.size());

    for (auto &c : chats) {
        long long id = c.first;
        const std::string &title = c.second;

        lv_obj_t *row = create_text(sub_page, NULL, title.c_str(),
                                    LV_MENU_ITEM_BUILDER_VARIANT_2);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(btn, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_FG, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_BG, LV_STATE_CHECKED);
        lv_obj_set_size(btn, 44, 28);
        lv_obj_set_flex_grow(btn, 0);

        bool starred = apps::tg_cfg_is_favorite(id);
        if (starred) lv_obj_add_state(btn, LV_STATE_CHECKED);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, starred ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
        lv_obj_center(label);
        lv_obj_set_user_data(btn, label);

        long long *id_heap = new long long(id);
        lv_obj_add_event_cb(btn, fav_toggle_cb, LV_EVENT_VALUE_CHANGED, id_heap);
        lv_obj_add_event_cb(btn, fav_id_delete_cb, LV_EVENT_DELETE, id_heap);
        register_subpage_group_obj(sub_page, btn);
    }
}

} // namespace telegram_cfg
