/**
 * @file      settings_connectivity.cpp
 * @brief     Settings » Connectivity subpage. Extracted from ui_settings.cpp;
 *            see settings_internal.h for the cross-TU contract.
 *
 * Toggles: WiFi, Bluetooth, Radio, NFC, Speaker, Haptic. Plus three
 * follow-up buttons whose visibility tracks the relevant toggle:
 *   - "WiFi Networks" / "Test Internet"  — shown iff WiFi on
 *   - "NFC Test"                         — shown iff NFC on
 * Toggling WiFi/NFC rebuilds the focus group in place via
 * activate_subpage_group so the newly-hidden-or-shown rows leave/join the
 * encoder nav chain at their registered position.
 */
#include "../ui_define.h"
#include "../hal/wireless.h"
#include "../core/scoped_lock.h"
#include "settings_internal.h"
#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace connectivity_cfg {

namespace {

// Connectivity subpage — "WiFi Networks" row is hidden when WiFi is off,
// "NFC Test" row the same when NFC is off. `row` is the full menu_cont
// (icon + label + right-arrow btn); `btn` is the inner clickable that needs
// to join/leave the nav group. Hiding the whole row drops the label too.
lv_obj_t *g_wifi_networks_row    = nullptr;
lv_obj_t *g_wifi_networks_btn    = nullptr;
lv_obj_t *g_nfc_test_row         = nullptr;
lv_obj_t *g_nfc_test_btn         = nullptr;
lv_obj_t *g_connectivity_subpage = nullptr;
lv_obj_t *g_internet_test_row    = nullptr;
lv_obj_t *g_internet_test_btn    = nullptr;
lv_obj_t *g_internet_test_status = nullptr;

void wifi_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.wifi_enable = en;
    hw_set_wifi_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
    if (g_wifi_networks_row) {
        if (en) {
            lv_obj_clear_flag(g_wifi_networks_row, LV_OBJ_FLAG_HIDDEN);
            if (g_wifi_networks_btn) lv_obj_clear_flag(g_wifi_networks_btn, LV_OBJ_FLAG_HIDDEN);
            if (g_internet_test_row) lv_obj_clear_flag(g_internet_test_row, LV_OBJ_FLAG_HIDDEN);
            if (g_internet_test_btn) lv_obj_clear_flag(g_internet_test_btn, LV_OBJ_FLAG_HIDDEN);
            if (g_internet_test_status) lv_obj_clear_flag(g_internet_test_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_wifi_networks_row, LV_OBJ_FLAG_HIDDEN);
            if (g_wifi_networks_btn) lv_obj_add_flag(g_wifi_networks_btn, LV_OBJ_FLAG_HIDDEN);
            if (g_internet_test_row) lv_obj_add_flag(g_internet_test_row, LV_OBJ_FLAG_HIDDEN);
            if (g_internet_test_btn) lv_obj_add_flag(g_internet_test_btn, LV_OBJ_FLAG_HIDDEN);
            if (g_internet_test_status) lv_obj_add_flag(g_internet_test_status, LV_OBJ_FLAG_HIDDEN);
        }
        // Rebuild the subpage nav group so the row is inserted at its
        // registered position (between the WiFi toggle and Bluetooth) rather
        // than tacked onto the end, and so it's actually focusable again.
        // activate_subpage_group defaults focus to the back button — move it
        // back to the WiFi toggle the user just interacted with.
        if (g_connectivity_subpage) {
            activate_subpage_group(g_connectivity_subpage);
            lv_group_focus_obj(obj);
        }
    }
}

void bt_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.bt_enable = en;
    hw_set_bt_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

void radio_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.radio_enable = en;
    int16_t st = hw_set_radio_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
    if (st != 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Radio config failed (err %d)", (int)st);
        ui_msg_pop_up("Radio", msg);
    }
}

void nfc_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.nfc_enable = en;
    hw_set_nfc_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
    if (g_nfc_test_row) {
        if (en) {
            lv_obj_clear_flag(g_nfc_test_row, LV_OBJ_FLAG_HIDDEN);
            if (g_nfc_test_btn) lv_obj_clear_flag(g_nfc_test_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_nfc_test_row, LV_OBJ_FLAG_HIDDEN);
            if (g_nfc_test_btn) lv_obj_add_flag(g_nfc_test_btn, LV_OBJ_FLAG_HIDDEN);
        }
        // Rebuild nav group so the row reclaims focusability when re-shown.
        if (g_connectivity_subpage) {
            activate_subpage_group(g_connectivity_subpage);
            lv_group_focus_obj(obj);
        }
    }
}

void speaker_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.speaker_enable = en;
    hw_set_speaker_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

void haptic_enable_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    local_param.haptic_enable = en;
    hw_set_haptic_enable(en);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    if (label) lv_label_set_text(label, en ? " On " : " Off ");
}

void wifi_networks_click_cb(lv_event_t *)
{
    ui_wifi_networks_open();
}

// Render the internet-test outcome into the status label. Shared by the
// async drain and the synchronous fallbacks (task-spawn failure, emulator).
void show_internet_result(bool ok, uint32_t rtt_ms, const std::string &err)
{
    if (!g_internet_test_status) return;
    if (ok) {
        char buf[48];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK " Online (%u ms)", (unsigned)rtt_ms);
        lv_label_set_text(g_internet_test_status, buf);
        lv_obj_set_style_text_color(g_internet_test_status,
                                    lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        std::string msg = LV_SYMBOL_CLOSE " " + (err.empty() ? std::string("Failed") : err);
        lv_label_set_text(g_internet_test_status, msg.c_str());
        lv_obj_set_style_text_color(g_internet_test_status,
                                    lv_palette_main(LV_PALETTE_RED), 0);
    }
}

#ifdef ARDUINO
// P2.3: hw_ping_internet blocks for up to 3 s; running it inline froze the
// LVGL thread (dead back button/encoder) for the whole probe — exactly the
// situation in which a user taps this button. Run it on a one-shot worker
// and deliver the result via a drain timer, same lifecycle contract as the
// weather city-search fetch (settings_weather.cpp): the worker touches only
// heap state under the instance lock (never the widget tree), and
// internet_test_teardown() kills the drain before the subpage widgets free.
//
// TODO(cleanup): this {task, drain-timer, volatile done, heap result*} scaffold
// is now the ~5th hand-rolled copy of the same idiom (menu_app.cpp,
// settings_weather.cpp, settings_telegram.cpp, ui_notes_sync.cpp,
// ui_telegram.cpp). Retire them into one shared core::UiAsyncProbe<T> helper in
// its own refactor. Kept deliberately verbatim-uniform with the weather twin
// here so that extraction stays mechanical — do not tidy this copy in isolation.
struct InternetTestResult {
    bool ok = false;
    uint32_t rtt_ms = 0;
    std::string err;
};
TaskHandle_t        g_inet_test_task   = nullptr;   // single worker slot
lv_timer_t         *g_inet_test_timer  = nullptr;
volatile bool       g_inet_test_done   = false;
InternetTestResult *g_inet_test_result = nullptr;   // heap; UI thread frees

void internet_test_task(void *arg)
{
    (void)arg;
    InternetTestResult *res = new InternetTestResult();
    res->ok = hw_ping_internet("1.1.1.1", 53, 3000, &res->rtt_ms, &res->err);
    {
        core::ScopedInstanceLock lock;
        if (g_inet_test_result) { delete g_inet_test_result; g_inet_test_result = nullptr; }
        g_inet_test_result = res;
        g_inet_test_done = true;
        g_inet_test_task = nullptr;
    }
    vTaskDelete(NULL);
}

void internet_test_drain_tick(lv_timer_t *t)
{
    (void)t;
    if (!g_inet_test_done) return;
    g_inet_test_done = false;
    InternetTestResult *res = g_inet_test_result;
    g_inet_test_result = nullptr;
    if (g_inet_test_timer) { lv_timer_del(g_inet_test_timer); g_inet_test_timer = nullptr; }
    if (!res) return;
    show_internet_result(res->ok, res->rtt_ms, res->err);
    delete res;
}

void internet_test_teardown()
{
    if (g_inet_test_timer) { lv_timer_del(g_inet_test_timer); g_inet_test_timer = nullptr; }
    if (g_inet_test_done) {
        g_inet_test_done = false;
        if (g_inet_test_result) { delete g_inet_test_result; g_inet_test_result = nullptr; }
    }
}
#endif // ARDUINO

void internet_test_click_cb(lv_event_t *)
{
    if (!g_internet_test_status) return;
#ifdef ARDUINO
    bool spawn;
    {
        core::ScopedInstanceLock lock;
        spawn = (g_inet_test_task == nullptr);   // else a prior check is still running
        if (spawn) g_inet_test_done = false;
    }
    if (!spawn) {
        // A check is already in flight (e.g. page was exited/re-entered mid-
        // probe, which tore down the old drain timer). Re-attach the drain
        // so that worker's result still lands rather than stranding it.
        if (!g_inet_test_timer)
            g_inet_test_timer = lv_timer_create(internet_test_drain_tick, 100, nullptr);
        lv_label_set_text(g_internet_test_status, "Still testing...");
        lv_obj_set_style_text_color(g_internet_test_status, UI_COLOR_ACCENT, 0);
        return;
    }

    lv_label_set_text(g_internet_test_status, "Testing 1.1.1.1...");
    lv_obj_set_style_text_color(g_internet_test_status, UI_COLOR_ACCENT, 0);
    if (xTaskCreate(internet_test_task, "inet_tst", 4096, nullptr, 1,
                    &g_inet_test_task) != pdPASS) {
        // Task slots exhausted — fall back to a synchronous probe so the
        // button still works (pays the old up-to-3 s UI freeze).
        g_inet_test_task = nullptr;
        uint32_t rtt_ms = 0;
        std::string err;
        bool ok = hw_ping_internet("1.1.1.1", 53, 3000, &rtt_ms, &err);
        show_internet_result(ok, rtt_ms, err);
        return;
    }
    if (!g_inet_test_timer)
        g_inet_test_timer = lv_timer_create(internet_test_drain_tick, 100, nullptr);
#else
    // Emulator: no FreeRTOS task; probe inline.
    lv_label_set_text(g_internet_test_status, "Testing 1.1.1.1...");
    lv_obj_set_style_text_color(g_internet_test_status, UI_COLOR_ACCENT, 0);
    lv_refr_now(NULL);
    uint32_t rtt_ms = 0;
    std::string err;
    bool ok = hw_ping_internet("1.1.1.1", 53, 3000, &rtt_ms, &err);
    show_internet_result(ok, rtt_ms, err);
#endif
}

} // anonymous namespace

void reset_state()
{
    g_wifi_networks_row    = nullptr;
    g_wifi_networks_btn    = nullptr;
    g_nfc_test_row         = nullptr;
    g_nfc_test_btn         = nullptr;
    g_connectivity_subpage = nullptr;
    g_internet_test_row    = nullptr;
    g_internet_test_btn    = nullptr;
    g_internet_test_status = nullptr;
#ifdef ARDUINO
    internet_test_teardown();
#endif
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    lv_obj_t *btn;

    btn = create_toggle_btn_row(sub_page, "WiFi", hw_get_wifi_enable(), wifi_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_button(sub_page, LV_SYMBOL_WIFI, "WiFi Networks", wifi_networks_click_cb);
    g_wifi_networks_btn = btn;
    g_wifi_networks_row = lv_obj_get_parent(btn);  // menu_cont holding icon+label+btn
    g_connectivity_subpage = sub_page;
    if (!hw_get_wifi_enable()) {
        lv_obj_add_flag(g_wifi_networks_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
    register_subpage_group_obj(sub_page, btn);

    // "Test Internet" — TCP connect to Cloudflare DNS (1.1.1.1:53) to confirm
    // the WiFi link has actual internet reachability, not just an AP association.
    // Hidden together with the WiFi Networks row when WiFi is off.
    btn = create_button(sub_page, LV_SYMBOL_REFRESH, "Test Internet", internet_test_click_cb);
    g_internet_test_btn = btn;
    g_internet_test_row = lv_obj_get_parent(btn);
    g_internet_test_status = lv_label_create(sub_page);
    lv_label_set_text(g_internet_test_status, "");
    lv_obj_set_style_text_color(g_internet_test_status, UI_COLOR_MUTED, 0);
    lv_obj_set_style_pad_left(g_internet_test_status, 12, 0);
    if (!hw_get_wifi_enable()) {
        lv_obj_add_flag(g_internet_test_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_internet_test_status, LV_OBJ_FLAG_HIDDEN);
    }
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "Bluetooth", hw_get_bt_enable(), bt_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "Radio", hw_get_radio_enable(), radio_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "NFC", hw_get_nfc_enable(), nfc_enable_cb);
    register_subpage_group_obj(sub_page, btn);

#if defined(USING_ST25R3916)
    btn = create_button(sub_page, LV_SYMBOL_REFRESH, "NFC Test",
                        [](lv_event_t *) { ui_nfc_test_open(); });
    g_nfc_test_btn = btn;
    g_nfc_test_row = lv_obj_get_parent(btn);
    if (!hw_get_nfc_enable()) {
        lv_obj_add_flag(g_nfc_test_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
    register_subpage_group_obj(sub_page, btn);
#endif

    btn = create_toggle_btn_row(sub_page, "Speaker", hw_get_speaker_enable(), speaker_enable_cb);
    register_subpage_group_obj(sub_page, btn);

    btn = create_toggle_btn_row(sub_page, "Haptic", hw_get_haptic_enable(), haptic_enable_cb);
    register_subpage_group_obj(sub_page, btn);
}

} // namespace connectivity_cfg
