/**
 * @file      settings_info.cpp
 * @brief     Settings » System Info subpage. Extracted from ui_settings.cpp;
 *            see settings_internal.h for the cross-TU contract.
 *
 * Read-only info rows: MAC, WiFi SSID/IP/RSSI, battery, SD + internal
 * storage usage, LVGL version, Arduino core version, build date, firmware
 * hash, chip id. A 1 Hz lv_timer refreshes the live rows (clock, RSSI,
 * battery voltage); storage + SSID + IP are loaded once on first tick
 * (gated by `info_loaded`) because they're expensive.
 */
#include "../ui_define.h"
#include "settings_internal.h"

namespace info_cfg {

namespace {

struct sys_label_t {
    lv_obj_t *datetime_label         = nullptr;
    lv_obj_t *wifi_rssi_label        = nullptr;
    lv_obj_t *batt_voltage_label     = nullptr;

    lv_obj_t *wifi_ssid_label        = nullptr;
    lv_obj_t *ip_info_label          = nullptr;
    lv_obj_t *sd_size_label          = nullptr;
    lv_obj_t *storage_used_label     = nullptr;
    lv_obj_t *storage_free_label     = nullptr;
    lv_obj_t *local_storage_total_label = nullptr;
    lv_obj_t *local_storage_used_label  = nullptr;
    lv_obj_t *local_storage_free_label  = nullptr;
    bool      info_loaded            = false;
};

sys_label_t sys_label;
lv_timer_t *info_timer = nullptr;

void sys_timer_event_cb(lv_timer_t *)
{
    std::string datetime;
    hw_get_date_time(datetime);
    lv_label_set_text_fmt(sys_label.datetime_label, "%s", datetime.c_str());

    if (hw_get_wifi_connected()) {
        lv_label_set_text_fmt(sys_label.wifi_rssi_label, "%d", hw_get_wifi_rssi());
    }
    lv_label_set_text_fmt(sys_label.batt_voltage_label, "%d mV", hw_get_battery_voltage());

    if (!sys_label.info_loaded) {
        std::string wifi_ssid = "N/A";
        hw_get_wifi_ssid(wifi_ssid);
        if (sys_label.wifi_ssid_label) lv_label_set_text(sys_label.wifi_ssid_label, wifi_ssid.c_str());

        std::string ip_info = "N/A";
        hw_get_ip_address(ip_info);
        if (sys_label.ip_info_label) lv_label_set_text(sys_label.ip_info_label, ip_info.c_str());

        uint64_t total = 0, used = 0, free = 0;
        hw_get_storage_info(total, used, free);
        char buffer[64];

        auto format_size = [](char *buf, size_t len, uint64_t bytes) {
            if (bytes > 1024ULL * 1024 * 1024) {
                snprintf(buf, len, "%.2f GB", bytes / 1024.0 / 1024.0 / 1024.0);
            } else if (bytes > 1024 * 1024) {
                snprintf(buf, len, "%.2f MB", bytes / 1024.0 / 1024.0);
            } else if (bytes > 1024) {
                snprintf(buf, len, "%.2f KB", bytes / 1024.0);
            } else {
                snprintf(buf, len, "%llu B", bytes);
            }
        };

        if (total > 0) {
            format_size(buffer, sizeof(buffer), total);
            if (sys_label.sd_size_label) lv_label_set_text(sys_label.sd_size_label, buffer);

            format_size(buffer, sizeof(buffer), used);
            if (sys_label.storage_used_label) lv_label_set_text(sys_label.storage_used_label, buffer);

            format_size(buffer, sizeof(buffer), free);
            if (sys_label.storage_free_label) lv_label_set_text(sys_label.storage_free_label, buffer);
        } else {
            if (sys_label.sd_size_label) lv_label_set_text(sys_label.sd_size_label, "N/A");
            if (sys_label.storage_used_label) lv_label_set_text(sys_label.storage_used_label, "N/A");
            if (sys_label.storage_free_label) lv_label_set_text(sys_label.storage_free_label, "N/A");
        }

        uint64_t l_total = 0, l_used = 0, l_free = 0;
        hw_get_local_storage_info(l_total, l_used, l_free);
        if (l_total > 0) {
            format_size(buffer, sizeof(buffer), l_total);
            if (sys_label.local_storage_total_label) lv_label_set_text(sys_label.local_storage_total_label, buffer);

            format_size(buffer, sizeof(buffer), l_used);
            if (sys_label.local_storage_used_label) lv_label_set_text(sys_label.local_storage_used_label, buffer);

            format_size(buffer, sizeof(buffer), l_free);
            if (sys_label.local_storage_free_label) lv_label_set_text(sys_label.local_storage_free_label, buffer);
        } else {
            if (sys_label.local_storage_total_label) lv_label_set_text(sys_label.local_storage_total_label, "N/A");
            if (sys_label.local_storage_used_label) lv_label_set_text(sys_label.local_storage_used_label, "N/A");
            if (sys_label.local_storage_free_label) lv_label_set_text(sys_label.local_storage_free_label, "N/A");
        }

        sys_label.info_loaded = true;
    }
}

} // anonymous namespace

void reset_state()
{
    if (info_timer) {
        lv_timer_del(info_timer);
        info_timer = nullptr;
    }
    sys_label = sys_label_t{};
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    sys_label.info_loaded = false;
    lv_obj_add_flag(sub_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sub_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sub_page, 4, 0);
    lv_obj_set_style_pad_row(sub_page, 0, 0);

    auto add_info_row = [&](const char *key, const char *val) -> lv_obj_t* {
        lv_obj_t *row = lv_menu_cont_create(sub_page);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
        lv_obj_set_style_radius(row, 0, 0);

        lv_obj_t *k = lv_label_create(row);
        lv_label_set_text(k, key);
        lv_obj_set_style_text_color(k, lv_palette_main(LV_PALETTE_GREY), 0);

        lv_obj_t *v = lv_label_create(row);
        lv_label_set_text(v, val);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(v, LV_PCT(55), 0);

        register_subpage_group_obj(sub_page, row);
        return v;
    };

    char buffer[128];
    uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    bool has_mac = hw_get_mac(mac);
    if (has_mac) {
        snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    add_info_row("MAC", has_mac ? buffer : "N/A");

    sys_label.wifi_ssid_label = add_info_row("WiFi", "Loading...");
    sys_label.datetime_label = add_info_row("RTC", "00:00:00");
    sys_label.ip_info_label = add_info_row("IP", "Loading...");
    sys_label.wifi_rssi_label = add_info_row("RSSI", "N/A");

    snprintf(buffer, sizeof(buffer), "%d mV", hw_get_battery_voltage());
    sys_label.batt_voltage_label = add_info_row("Battery", buffer);

#if defined(HAS_SD_CARD_SOCKET)
    const char *storage_name = "SD Total";
#else
    const char *storage_name = "Storage Total";
#endif
    sys_label.sd_size_label = add_info_row(storage_name, "Loading...");
    sys_label.storage_used_label = add_info_row("Storage Used", "Loading...");
    sys_label.storage_free_label = add_info_row("Storage Free", "Loading...");

    sys_label.local_storage_total_label = add_info_row("Internal Total", "Loading...");
    sys_label.local_storage_used_label = add_info_row("Internal Used", "Loading...");
    sys_label.local_storage_free_label = add_info_row("Internal Free", "Loading...");

    snprintf(buffer, sizeof(buffer), "%d.%d.%d", lv_version_major(), lv_version_minor(), lv_version_patch());
    add_info_row("LVGL", buffer);

    std::string ver;
    hw_get_arduino_version(ver);
    add_info_row("Core", ver.c_str());

    add_info_row("Built", __DATE__);
    add_info_row("Hash", hw_get_firmware_hash_string());
    add_info_row("Chip", hw_get_chip_id_string());

    // Kill any prior timer (e.g. if build_subpage runs a second time on
    // settings_page_changed_cb rebuild) before starting a new one.
    if (info_timer) lv_timer_del(info_timer);
    info_timer = lv_timer_create(sys_timer_event_cb, 1000, NULL);
}

} // namespace info_cfg
