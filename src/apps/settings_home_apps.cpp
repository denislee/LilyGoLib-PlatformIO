/**
 * @file      settings_home_apps.cpp
 * @brief     Settings » Home Apps subpage. Per-tile visibility toggles for
 *            the home-screen grid; backed by apps::home_apps_* (NVS).
 *            See settings_internal.h for the cross-TU contract.
 *
 * Hidden tiles are skipped at render time in MenuApp::onStart — the apps
 * themselves stay registered and remain reachable via Settings shortcuts
 * and direct AppManager::switchApp calls.
 */
#include "../ui_define.h"
#include "menu_app.h"
#include "settings_internal.h"

namespace home_apps_cfg {

namespace {

void toggle_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    apps::home_apps_set_visible(idx, en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

} // anonymous namespace

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    int n = apps::home_apps_count();
    for (int i = 0; i < n; i++) {
        const char *label = apps::home_apps_label(i);
        bool on = apps::home_apps_is_visible(i);
        lv_obj_t *btn = create_toggle_btn_row(sub_page, label, on, toggle_cb);
        // Stash the index so the single callback can route back to the
        // right entry — cheaper than wrapping a struct per row.
        lv_obj_remove_event_cb(btn, toggle_cb);
        lv_obj_add_event_cb(btn, toggle_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);
        register_subpage_group_obj(sub_page, btn);
    }
}

} // namespace home_apps_cfg
