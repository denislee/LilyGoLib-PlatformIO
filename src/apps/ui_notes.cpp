/**
 * @file      ui_notes.cpp
 * @brief     Simple launcher presenting "New Note" and "Journal" — the two
 *            note-centric apps that used to live side-by-side on the home
 *            grid. Keeps the home screen compact and groups journal browsing
 *            next to note creation where users go looking for it.
 */
#include "../ui_define.h"
#include "../core/app_manager.h"
#include "../core/system.h"
#include "app_registry.h"

namespace {

struct NotesItem {
    const char *label;
    const char *symbol;
    const char *appName;
    lv_palette_t palette;
};

static const NotesItem kItems[] = {
    {"New Note", LV_SYMBOL_EDIT,    "Editor",     LV_PALETTE_ORANGE},
    {"Journal",  LV_SYMBOL_BARS,    "Journal",    LV_PALETTE_CYAN},
    {"Sync",     LV_SYMBOL_REFRESH, "Notes Sync", LV_PALETTE_GREEN},
};
constexpr int kItemCount = sizeof(kItems) / sizeof(kItems[0]);

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    menu_show();
}

static void tile_click_cb(lv_event_t *e)
{
    const NotesItem *item = (const NotesItem *)lv_event_get_user_data(e);
    if (!item) return;
    hw_feedback();
    core::AppManager::getInstance().switchApp(item->appName,
        core::System::getInstance().getAppPanel());
}

static void ui_notes_enter(lv_obj_t *parent)
{
    ui_show_back_button(back_btn_cb);

    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 8, 0);
    lv_obj_set_style_pad_column(parent, 8, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_update_layout(parent);
    int32_t panel_w = lv_obj_get_content_width(parent);
    int32_t panel_h = lv_obj_get_content_height(parent);
    if (panel_w <= 0) panel_w = 460;
    if (panel_h <= 0) panel_h = 180;

    const int gap = 8;
    int32_t tile_w = (panel_w - (kItemCount - 1) * gap) / kItemCount;
    int32_t tile_h = panel_h;
    if (tile_w < 50) tile_w = 50;
    if (tile_h < 50) tile_h = 50;

    const lv_font_t *icon_font =
        (tile_h >= 140) ? &lv_font_montserrat_48 :
        (tile_h >= 90)  ? &lv_font_montserrat_32 :
        (tile_h >= 70)  ? &lv_font_montserrat_28 :
                          &lv_font_montserrat_24;
    const lv_font_t *label_font = get_home_font();
    lv_group_t *grp = lv_group_get_default();

    for (int i = 0; i < kItemCount; ++i) {
        const NotesItem &item = kItems[i];
        lv_color_t accent = lv_palette_main(item.palette);

        lv_obj_t *tile = lv_btn_create(parent);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(tile, 12, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x151515), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, accent, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_40, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_outline_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 6, 0);
        lv_obj_set_style_border_color(tile, accent, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(tile, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(tile, lv_palette_darken(item.palette, 4),
                                  LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(tile, lv_palette_darken(item.palette, 3),
                                  LV_STATE_PRESSED);

        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tile, 4, 0);

        lv_obj_t *icon = lv_label_create(tile);
        lv_label_set_text(icon, item.symbol);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, accent, 0);

        lv_obj_t *label = lv_label_create(tile);
        lv_label_set_text(label, item.label);
        lv_obj_set_style_text_color(label, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(label, label_font, 0);
        lv_obj_set_width(label, tile_w - 12);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED,
                            (void *)&kItems[i]);
        if (grp) lv_group_add_obj(grp, tile);
    }

    if (grp && lv_obj_get_child_count(parent) > 0) {
        lv_group_focus_obj(lv_obj_get_child(parent, 0));
    }
}

static void ui_notes_exit(lv_obj_t *parent)
{
    (void)parent;
    ui_hide_back_button();
}

class NotesMenuApp : public core::App {
public:
    NotesMenuApp() : core::App("Notes") {}
    void onStart(lv_obj_t *parent) override {
        setRoot(parent);
        ui_notes_enter(parent);
    }
    void onStop() override {
        ui_notes_exit(getRoot());
        core::App::onStop();
    }
};

} // namespace

namespace apps {
APP_FACTORY(make_notes_menu_app, NotesMenuApp)
} // namespace apps
