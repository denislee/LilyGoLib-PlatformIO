/**
 * @file      settings_imu_debug.cpp
 * @brief     Settings » IMU Debug subpage. Read-only diagnostic view of the
 *            BHI260AP (or BMA423) state, intended for figuring out why
 *            face-down detection isn't triggering on a given board.
 *
 * Surfaces every gating signal in one screen:
 *   - Device-probe online flag (HW_BHI260AP_ONLINE / HW_BMA423_ONLINE).
 *   - Firmware identity from BoschSensorInfo: kernel / user / ROM version,
 *     product id, boot/host/feat status, sensor_error + decoded text.
 *   - Available virtual sensor count.
 *   - Per virtual sensor: configure() success + live event counter.
 *   - Live values: ax/ay/az (g), roll/pitch/yaw (deg), device-orientation
 *     byte, BMA423 direction enum.
 *   - Computed face-down result.
 *   - Actions: "Re-init IMU" (unregister + register without reboot) and
 *     "Print to Serial" (dump full firmware sensor table over UART).
 *
 * A 200 ms lv_timer drives the live rows; reset_state() kills the timer
 * when the page is torn down.
 */
#include "../ui_define.h"
#include "settings_internal.h"
#include "../hal/sensors.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

namespace imu_debug_cfg {

namespace {

struct labels_t {
    lv_obj_t *probe_hex    = nullptr;
    lv_obj_t *probe_decode = nullptr;
    lv_obj_t *i2c_scan     = nullptr;
    lv_obj_t *online       = nullptr;
    lv_obj_t *fw_kernel    = nullptr;
    lv_obj_t *fw_user      = nullptr;
    lv_obj_t *fw_rom       = nullptr;
    lv_obj_t *fw_product   = nullptr;
    lv_obj_t *fw_boot      = nullptr;
    lv_obj_t *fw_host      = nullptr;
    lv_obj_t *fw_feat      = nullptr;
    lv_obj_t *fw_err       = nullptr;
    lv_obj_t *count        = nullptr;
    lv_obj_t *accel        = nullptr;
    lv_obj_t *grv          = nullptr;
    lv_obj_t *dev_orient   = nullptr;
    lv_obj_t *accel_xyz    = nullptr;
    lv_obj_t *roll_pitch   = nullptr;
    lv_obj_t *do_byte      = nullptr;
    lv_obj_t *face_down    = nullptr;
};

labels_t lbl;
lv_timer_t *tick = nullptr;
// Captured in build_subpage so render_tick can skip its I2C reads while this
// subpage is off-screen — lv_menu keeps the built page (and this timer) alive
// after the user navigates away.
lv_obj_t *s_menu = nullptr;
lv_obj_t *s_page = nullptr;

lv_obj_t *add_row(lv_obj_t *parent, const char *key, const char *val)
{
    lv_obj_t *row = lv_menu_cont_create(parent);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
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
    lv_obj_set_style_max_width(v, LV_PCT(60), 0);

    register_subpage_group_obj(parent, row);
    return v;
}

void reinit_cb(lv_event_t *)
{
    hw_unregister_imu_process();
    hw_register_imu_process();
    ui_msg_pop_up("IMU", "Re-init requested. Watch the rows for events.");
}

// Direct sensor.begin(Wire) bypassing LilyGoLib. Reports the boolean result
// straight from the SensorBHI260AP driver — the most reliable way to see
// whether the chip itself answers on the I2C bus.
void try_begin_cb(lv_event_t *)
{
#ifdef ARDUINO
    Wire.setClock(1000000UL);
    bool ok = instance.sensor.begin(Wire);
    Wire.setClock(400000UL);
    char msg[96];
    snprintf(msg, sizeof(msg),
             "sensor.begin(Wire) = %s\nIf false: chip not responding at 0x28",
             ok ? "TRUE" : "FALSE");
    Serial.println(msg);
    ui_msg_pop_up("IMU", msg);
#else
    ui_msg_pop_up("IMU", "Emulator: no hardware.");
#endif
}

// Probe each I2C address we care about. Build a comma-separated list of the
// ones that ACK so the user can see whether the BHI260 (0x28) and / or
// BMA423 (0x18) are present on the bus.
void rescan_i2c(char *out, size_t cap)
{
    out[0] = 0;
#ifdef ARDUINO
    const uint8_t addrs[] = { 0x18, 0x19, 0x28, 0x29 };
    bool first = true;
    for (uint8_t a : addrs) {
        Wire.beginTransmission(a);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%s0x%02X", first ? "" : ",", a);
            strncat(out, tmp, cap - strlen(out) - 1);
            first = false;
        }
    }
    if (first) strncat(out, "(none)", cap - strlen(out) - 1);
#else
    strncat(out, "(emu)", cap - strlen(out) - 1);
#endif
}

void serial_dump_cb(lv_event_t *)
{
#ifdef ARDUINO
    if (hw_get_device_online() & HW_BHI260AP_ONLINE) {
        BoschSensorInfo info = instance.sensor.getSensorInfo();
        info.printInfo(Serial);
        Serial.println("--- IMU diag dump ---");
        imu_diag_t d;
        hw_get_imu_diag(d);
        Serial.printf("bhi260_online=%d sensor_count=%u\n",
                      (int)d.bhi260_online, (unsigned)d.sensor_count);
        Serial.printf("accel  cfg=%d events=%u  ax=%.3f ay=%.3f az=%.3f\n",
                      (int)d.accel_configured, (unsigned)d.accel_events,
                      (double)d.accel_g_x, (double)d.accel_g_y,
                      (double)d.accel_g_z);
        Serial.printf("grv    cfg=%d events=%u\n",
                      (int)d.grv_configured, (unsigned)d.grv_events);
        Serial.printf("dorien cfg=%d events=%u byte=%u\n",
                      (int)d.dev_orient_configured,
                      (unsigned)d.dev_orient_events,
                      (unsigned)d.dev_orient_value);
        ui_msg_pop_up("IMU", "Dumped to serial @ 115200.");
        return;
    }
#endif
    ui_msg_pop_up("IMU", "BHI260 not online — nothing to dump.");
}

void render_tick(lv_timer_t *)
{
    // Only poll the IMU while this page is actually on screen. Without this the
    // BHI260 got read at 5 Hz the whole time the user browsed other settings
    // subpages after visiting this one once. Fully torn down on Settings exit.
    if (!s_menu || lv_menu_get_cur_main_page(s_menu) != s_page) return;

    char buf[96];

    imu_diag_t d;
    hw_get_imu_diag(d);
    imu_params_t p;
    hw_get_imu_params(p);
    bool fd = hw_is_face_down();

    if (lbl.online) {
        snprintf(buf, sizeof(buf), "BHI260=%s  BMA423=%s",
                 d.bhi260_online ? "yes" : "no",
                 d.bma423_online ? "yes" : "no");
        lv_label_set_text(lbl.online, buf);
    }

#ifdef ARDUINO
    // Raw devices_probe value + decode of the bits we care about. If the IMU
    // bits aren't set this tells us at a glance which OTHER peripherals did
    // come up — useful for distinguishing "init failed" from "code path
    // never ran".
    uint32_t probe = hw_get_device_online();
    if (lbl.probe_hex) {
        snprintf(buf, sizeof(buf), "0x%08X", (unsigned)probe);
        lv_label_set_text(lbl.probe_hex, buf);
    }
    if (lbl.probe_decode) {
        snprintf(buf, sizeof(buf),
                 "%s%s%s%s%s%s%s%s%s%s",
                 (probe & HW_PMU_ONLINE)      ? "PMU "   : "",
                 (probe & HW_RTC_ONLINE)      ? "RTC "   : "",
                 (probe & HW_SD_ONLINE)       ? "SD "    : "",
                 (probe & HW_NFC_ONLINE)      ? "NFC "   : "",
                 (probe & HW_BHI260AP_ONLINE) ? "BHI "   : "",
                 (probe & HW_KEYBOARD_ONLINE) ? "KB "    : "",
                 (probe & HW_BMA423_ONLINE)   ? "BMA "   : "",
                 (probe & HW_EXPAND_ONLINE)   ? "EXP "   : "",
                 (probe & HW_CODEC_ONLINE)    ? "AUD "   : "",
                 (probe & HW_GAUGE_ONLINE)    ? "GAU "   : "");
        lv_label_set_text(lbl.probe_decode, buf);
    }
#endif

#ifdef ARDUINO
    // Pull a fresh BoschSensorInfo each tick — cheap. Use the LIVE probe
    // bit rather than the cached diag flag so a failed firmware swap (which
    // can leave the chip in a transient state) doesn't blank these rows.
    if (hw_get_device_online() & HW_BHI260AP_ONLINE) {
        BoschSensorInfo info = instance.sensor.getSensorInfo();
        if (lbl.fw_kernel) {
            snprintf(buf, sizeof(buf), "%u", info.kernel_version);
            lv_label_set_text(lbl.fw_kernel, buf);
        }
        if (lbl.fw_user) {
            snprintf(buf, sizeof(buf), "%u", info.user_version);
            lv_label_set_text(lbl.fw_user, buf);
        }
        if (lbl.fw_rom) {
            snprintf(buf, sizeof(buf), "%u", info.rom_version);
            lv_label_set_text(lbl.fw_rom, buf);
        }
        if (lbl.fw_product) {
            snprintf(buf, sizeof(buf), "0x%02X", info.product_id);
            lv_label_set_text(lbl.fw_product, buf);
        }
        if (lbl.fw_boot) {
            snprintf(buf, sizeof(buf), "0x%02X", info.boot_status);
            lv_label_set_text(lbl.fw_boot, buf);
        }
        if (lbl.fw_host) {
            snprintf(buf, sizeof(buf), "0x%02X", info.host_status);
            lv_label_set_text(lbl.fw_host, buf);
        }
        if (lbl.fw_feat) {
            snprintf(buf, sizeof(buf), "0x%02X", info.feat_status);
            lv_label_set_text(lbl.fw_feat, buf);
        }
        if (lbl.fw_err) {
            // Pinned SensorLib has no getErrorText(); just show the raw byte.
            // 0x00 = no error. Any non-zero value is bad — printInfo() on
            // serial decodes it.
            snprintf(buf, sizeof(buf), "0x%02X", info.sensor_error);
            lv_label_set_text(lbl.fw_err, buf);
        }
    }
#endif

    if (lbl.count) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)d.sensor_count);
        lv_label_set_text(lbl.count, buf);
    }
    if (lbl.accel) {
        snprintf(buf, sizeof(buf), "cfg=%d  evt=%u",
                 (int)d.accel_configured, (unsigned)d.accel_events);
        lv_label_set_text(lbl.accel, buf);
    }
    if (lbl.grv) {
        snprintf(buf, sizeof(buf), "cfg=%d  evt=%u",
                 (int)d.grv_configured, (unsigned)d.grv_events);
        lv_label_set_text(lbl.grv, buf);
    }
    if (lbl.dev_orient) {
        snprintf(buf, sizeof(buf), "cfg=%d  evt=%u",
                 (int)d.dev_orient_configured,
                 (unsigned)d.dev_orient_events);
        lv_label_set_text(lbl.dev_orient, buf);
    }
    if (lbl.accel_xyz) {
        snprintf(buf, sizeof(buf), "%+.2f  %+.2f  %+.2f",
                 (double)d.accel_g_x, (double)d.accel_g_y,
                 (double)d.accel_g_z);
        lv_label_set_text(lbl.accel_xyz, buf);
    }
    if (lbl.roll_pitch) {
        snprintf(buf, sizeof(buf), "r=%+.0f  p=%+.0f  y=%+.0f",
                 (double)p.roll, (double)p.pitch, (double)p.heading);
        lv_label_set_text(lbl.roll_pitch, buf);
    }
    if (lbl.do_byte) {
        snprintf(buf, sizeof(buf), "%u  (orient=%u)",
                 (unsigned)d.dev_orient_value, (unsigned)p.orientation);
        lv_label_set_text(lbl.do_byte, buf);
    }
    if (lbl.face_down) {
        lv_label_set_text(lbl.face_down, fd ? "YES" : "no");
        lv_obj_set_style_text_color(lbl.face_down,
                                    fd ? UI_COLOR_ACCENT : UI_COLOR_FG, 0);
    }
}

} // anonymous namespace

void reset_state()
{
    if (tick) {
        lv_timer_del(tick);
        tick = nullptr;
    }
    s_menu = nullptr;
    s_page = nullptr;
    lbl = labels_t{};
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    s_menu = menu;
    s_page = sub_page;
    lv_obj_add_flag(sub_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sub_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sub_page, 4, 0);
    lv_obj_set_style_pad_row(sub_page, 0, 0);

    auto add_section = [&](const char *title) {
        lv_obj_t *row = lv_obj_create(sub_page);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, UI_COLOR_ACCENT, 0);
    };

    add_section("Detection");
    lbl.online        = add_row(sub_page, "Online", "...");
    lbl.probe_hex     = add_row(sub_page, "Probe raw", "...");
    lbl.probe_decode  = add_row(sub_page, "Probe bits", "...");
    {
        char scan[48];
        rescan_i2c(scan, sizeof(scan));
        lbl.i2c_scan = add_row(sub_page, "I2C @ 0x18/28", scan);
    }

    add_section("Firmware");
    lbl.fw_kernel  = add_row(sub_page, "Kernel ver", "...");
    lbl.fw_user    = add_row(sub_page, "User ver", "...");
    lbl.fw_rom     = add_row(sub_page, "ROM ver", "...");
    lbl.fw_product = add_row(sub_page, "Product ID", "...");
    lbl.fw_boot    = add_row(sub_page, "Boot status", "...");
    lbl.fw_host    = add_row(sub_page, "Host status", "...");
    lbl.fw_feat    = add_row(sub_page, "Feat status", "...");
    lbl.fw_err     = add_row(sub_page, "Sensor err", "...");

    add_section("Virtual sensors");
    lbl.count      = add_row(sub_page, "Count", "...");
    lbl.accel      = add_row(sub_page, "Accel (1)", "...");
    lbl.grv        = add_row(sub_page, "Game RV (37)", "...");
    lbl.dev_orient = add_row(sub_page, "Dev Orient (69)", "...");

    add_section("Live values");
    lbl.accel_xyz  = add_row(sub_page, "ax/ay/az (g)", "...");
    lbl.roll_pitch = add_row(sub_page, "roll/pitch/yaw", "...");
    lbl.do_byte    = add_row(sub_page, "DO byte", "...");
    lbl.face_down  = add_row(sub_page, "Face down?", "no");

    add_section("Actions");
    {
        lv_obj_t *row = create_text(sub_page, NULL, "Re-init IMU",
                                    LV_MENU_ITEM_BUILDER_VARIANT_2);
        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_width(btn, 70);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, "Run");
        lv_obj_center(lab);
        lv_obj_add_event_cb(btn, reinit_cb, LV_EVENT_CLICKED, NULL);
        register_subpage_group_obj(sub_page, btn);
    }
    {
        lv_obj_t *row = create_text(sub_page, NULL, "Print to Serial",
                                    LV_MENU_ITEM_BUILDER_VARIANT_2);
        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_width(btn, 70);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, "Dump");
        lv_obj_center(lab);
        lv_obj_add_event_cb(btn, serial_dump_cb, LV_EVENT_CLICKED, NULL);
        register_subpage_group_obj(sub_page, btn);
    }
    {
        lv_obj_t *row = create_text(sub_page, NULL, "Try sensor.begin",
                                    LV_MENU_ITEM_BUILDER_VARIANT_2);
        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_width(btn, 70);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, "Try");
        lv_obj_center(lab);
        lv_obj_add_event_cb(btn, try_begin_cb, LV_EVENT_CLICKED, NULL);
        register_subpage_group_obj(sub_page, btn);
    }

    if (tick) lv_timer_del(tick);
    tick = lv_timer_create(render_tick, 200, NULL);
    render_tick(tick);  // paint immediately so the page isn't full of "..."
}

} // namespace imu_debug_cfg
