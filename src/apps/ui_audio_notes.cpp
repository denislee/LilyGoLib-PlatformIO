/**
 * @file      ui_audio_notes.cpp
 * @brief     Audio "mental notes" app — record voice memos to SD, play them
 *            back, delete. All files live in /mental_notes/ on the SD card.
 */
#include "../ui_define.h"
#include "app_registry.h"
#include "../core/app_manager.h"
#include "../core/spi_lock.h"
#include "../hal/system.h"
#include "../hal/storage.h"
#include "../hal/audio.h"
#include <vector>
#include <string>
#include <algorithm>

#ifdef ARDUINO
#include <SD.h>
#include <LilyGoLib.h>
#endif

namespace {

static const char *NOTES_DIR = "/mental_notes";

enum ViewMode {
    VIEW_MAIN,
    VIEW_LIST,
    VIEW_RECORD,
    VIEW_PLAYBACK,
};

struct Note {
    std::string filename;   // leaf, e.g. "note_20260418_120530.wav"
    std::string full_path;  // "/mental_notes/note_20260418_120530.wav"
    uint32_t    mtime;
    uint32_t    data_bytes; // file size minus 44-byte header (for duration)
};

static lv_obj_t *menu = NULL;
static lv_obj_t *parent_obj = NULL;
static lv_obj_t *main_page = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_timer_t *tick_timer = NULL;

static ViewMode current_view = VIEW_MAIN;
static std::vector<Note> notes;
static std::string selected_path;   // for playback view
static std::string selected_name;
static uint32_t    selected_bytes = 0;
static uint32_t    selected_mtime = 0;
static std::string pending_rec_path; // set while recording
// Quick-record mode: home-screen shortcut launches us straight into a
// recording. Stop / back / auto-cap exits the app instead of falling back
// to the Record/Browse menu (which lives under Settings → Recordings).
static bool        s_quick_record = false;

static void ui_notes_enter(lv_obj_t *parent);
static void ui_notes_exit(lv_obj_t *parent);
static void render_view();
static void enter_main();
static void enter_list();
static void enter_record();
static void enter_playback(const Note &n);

// --- helpers -----------------------------------------------------------

static std::string format_duration_s(uint32_t seconds)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u:%02u",
             (unsigned)(seconds / 60), (unsigned)(seconds % 60));
    return buf;
}

static std::string format_date(uint32_t t)
{
    if (t == 0) return "-";
    time_t tt = (time_t)t;
    struct tm info;
#ifdef ARDUINO
    localtime_r(&tt, &info);
#else
    struct tm *p = localtime(&tt);
    if (!p) return "-";
    info = *p;
#endif
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             info.tm_year + 1900, info.tm_mon + 1, info.tm_mday);
    return buf;
}

static uint32_t bytes_to_seconds(uint32_t data_bytes)
{
    if (HW_REC_BYTES_PER_SEC == 0) return 0;
    return data_bytes / HW_REC_BYTES_PER_SEC;
}

static void ensure_notes_dir()
{
#ifdef ARDUINO
    if (!(HW_SD_ONLINE & hw_get_device_online())) return;
    core::ScopedSpiLock lock;
    if (!SD.exists(NOTES_DIR)) SD.mkdir(NOTES_DIR);
#endif
}

static std::string build_new_note_path()
{
    struct tm info = {};
    hw_get_date_time(info);
    char buf[64];
    snprintf(buf, sizeof(buf),
             "%s/note_%04d%02d%02d_%02d%02d%02d.wav",
             NOTES_DIR,
             info.tm_year + 1900, info.tm_mon + 1, info.tm_mday,
             info.tm_hour, info.tm_min, info.tm_sec);
    return buf;
}

static void reload_notes()
{
    notes.clear();
    std::vector<HwDirEntry> raw;
    hw_list_sd_entries(raw, ".wav", NOTES_DIR);
    for (const auto &r : raw) {
        if (r.is_dir) continue;
        Note n;
        n.filename  = r.path;
        n.full_path = std::string(NOTES_DIR) + "/" + r.path;
        n.mtime     = r.mtime;
        // File size — re-open briefly. Could be slow; we cache it.
        n.data_bytes = 0;
#ifdef ARDUINO
        {
            core::ScopedSpiLock lock;
            File f = SD.open(n.full_path.c_str());
            if (f) {
                size_t sz = f.size();
                n.data_bytes = (sz >= 44) ? (uint32_t)(sz - 44) : 0;
                f.close();
            }
        }
#endif
        notes.push_back(n);
    }
    // Sort newest first by mtime, tie-break on filename desc.
    std::sort(notes.begin(), notes.end(), [](const Note &a, const Note &b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return a.filename > b.filename;
    });
}

// --- main view ---------------------------------------------------------

static void record_btn_cb(lv_event_t *e) { enter_record(); }

static void browse_btn_cb(lv_event_t *e) { enter_list(); }

struct MainTile {
    const char *label;
    const char *symbol;
    lv_palette_t palette;
    lv_event_cb_t cb;
};

static const MainTile kMainTiles[] = {
    {"Record", LV_SYMBOL_AUDIO, LV_PALETTE_RED,    record_btn_cb},
    {"Browse", LV_SYMBOL_LIST,  LV_PALETTE_ORANGE, browse_btn_cb},
};
constexpr int kMainTileCount = sizeof(kMainTiles) / sizeof(kMainTiles[0]);

static void build_main_view()
{
    bool mic_ok = hw_mic_available();

    // Horizontal tile row matching the "Notes" launcher app.
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main_page, 8, 0);
    lv_obj_set_style_pad_row(main_page, 8, 0);
    lv_obj_set_style_pad_column(main_page, 8, 0);
    lv_obj_remove_flag(main_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_update_layout(main_page);
    int32_t panel_w = lv_obj_get_content_width(main_page);
    int32_t panel_h = lv_obj_get_content_height(main_page);
    if (panel_w <= 0) panel_w = 460;
    if (panel_h <= 0) panel_h = 180;

    const int gap = 8;
    int32_t tile_w = (panel_w - (kMainTileCount - 1) * gap) / kMainTileCount;
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

    lv_obj_t *first_enabled = NULL;

    for (int i = 0; i < kMainTileCount; ++i) {
        const MainTile &item = kMainTiles[i];
        lv_color_t accent = lv_palette_main(item.palette);

        lv_obj_t *tile = lv_btn_create(main_page);
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

        // The Record tile is disabled if no microphone is present.
        bool disabled = (item.cb == record_btn_cb && !mic_ok);
        const char *symbol_text = disabled ? LV_SYMBOL_WARNING : item.symbol;
        const char *label_text  = disabled ? "No mic"          : item.label;
        if (disabled) lv_obj_add_state(tile, LV_STATE_DISABLED);

        lv_obj_t *icon = lv_label_create(tile);
        lv_label_set_text(icon, symbol_text);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, accent, 0);

        lv_obj_t *label = lv_label_create(tile);
        lv_label_set_text(label, label_text);
        lv_obj_set_style_text_color(label, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(label, label_font, 0);
        lv_obj_set_width(label, tile_w - 12);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        if (!disabled) {
            lv_obj_add_event_cb(tile, item.cb, LV_EVENT_CLICKED, NULL);
            if (!first_enabled) first_enabled = tile;
        }
        if (grp) lv_group_add_obj(grp, tile);
    }

    if (grp) {
        if (first_enabled) {
            lv_group_focus_obj(first_enabled);
        } else if (lv_obj_get_child_count(main_page) > 0) {
            lv_group_focus_obj(lv_obj_get_child(main_page, 0));
        }
    }
}

// --- list view ---------------------------------------------------------

static void note_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= (int)notes.size()) return;
    enter_playback(notes[idx]);
}

static void build_list_view()
{
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(main_page, 6, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    // Notes list.
    lv_obj_t *list = lv_list_create(main_page);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_radius(list, UI_RADIUS, 0);

    if (notes.empty()) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "No notes yet");
        lv_obj_set_style_text_color(empty, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_pad_all(empty, 16, 0);
    } else {
        for (size_t i = 0; i < notes.size(); ++i) {
            const Note &n = notes[i];
            lv_obj_t *btn = lv_list_add_btn(list, LV_SYMBOL_AUDIO, n.filename.c_str());
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, note_click_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), btn);

            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_t *icon_obj = lv_obj_get_child(btn, 0);
            lv_obj_t *lbl      = lv_obj_get_child(btn, 1);
            if (icon_obj) lv_obj_set_width(icon_obj, 20);
            if (lbl) {
                lv_obj_set_width(lbl, 0);
                lv_obj_set_flex_grow(lbl, 1);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            }

            // Trailing: duration + date.
            lv_obj_t *meta = lv_label_create(btn);
            std::string txt = format_duration_s(bytes_to_seconds(n.data_bytes))
                            + "  " + format_date(n.mtime);
            lv_label_set_text(meta, txt.c_str());
            lv_obj_set_style_text_color(meta, UI_COLOR_MUTED, 0);
            lv_obj_set_style_text_font(meta, get_small_font(), 0);
            lv_obj_set_style_pad_left(meta, 8, 0);
        }
    }

    if (!notes.empty()) {
        lv_group_focus_obj(lv_obj_get_child(list, 0));
    }
}

// --- record view -------------------------------------------------------

static lv_obj_t *rec_time_lbl = NULL;
static lv_obj_t *rec_status_lbl = NULL;
static lv_obj_t *rec_size_lbl = NULL;
static lv_obj_t *rec_dot = NULL;
static lv_obj_t *rec_bar = NULL;

static void exit_to_menu()
{
    if (hw_rec_running()) hw_rec_stop();
    hw_set_play_stop();
    ui_notes_exit(NULL);
    menu_show();
}

static void stop_rec_and_return(lv_event_t *e)
{
    if (hw_rec_running()) {
        hw_rec_stop();
    }
    if (s_quick_record) {
        exit_to_menu();
        return;
    }
    enter_main();
}

static void rec_tick_cb(lv_timer_t *t)
{
    if (!rec_time_lbl) return;
    uint32_t ms = hw_rec_elapsed_ms();
    uint32_t secs = ms / 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u",
             (unsigned)(secs / 60), (unsigned)(secs % 60));
    lv_label_set_text(rec_time_lbl, buf);

    if (rec_bar) {
        lv_bar_set_value(rec_bar, (int32_t)secs, LV_ANIM_OFF);
    }

    if (rec_size_lbl) {
        uint32_t bytes = hw_rec_bytes_written();
        char sbuf[48];
        snprintf(sbuf, sizeof(sbuf), "max %u:%02u  -  %u KB",
                 (unsigned)(HW_REC_MAX_MS / 60000),
                 (unsigned)((HW_REC_MAX_MS / 1000) % 60),
                 (unsigned)(bytes / 1024));
        lv_label_set_text(rec_size_lbl, sbuf);
    }

    if (!hw_rec_running()) {
        // Auto-stop (cap hit). Quick-record exits the app; the full app
        // returns to its Record/Browse menu.
        if (s_quick_record) {
            exit_to_menu();
            return;
        }
        enter_main();
    }
}

static void rec_dot_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void build_record_view()
{
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main_page, 8, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);
    lv_obj_remove_flag(main_page, LV_OBJ_FLAG_SCROLLABLE);

    // Header: pulsing red dot + "REC".
    lv_obj_t *hdr = lv_obj_create(main_page);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    rec_dot = lv_obj_create(hdr);
    lv_obj_remove_style_all(rec_dot);
    lv_obj_set_size(rec_dot, 14, 14);
    lv_obj_set_style_radius(rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rec_dot, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(rec_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(rec_dot, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_shadow_width(rec_dot, 10, 0);
    lv_obj_set_style_shadow_opa(rec_dot, LV_OPA_50, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rec_dot);
    lv_anim_set_exec_cb(&a, rec_dot_anim_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_time(&a, 600);
    lv_anim_set_playback_time(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    rec_status_lbl = lv_label_create(hdr);
    lv_label_set_text(rec_status_lbl, "REC");
    lv_obj_set_style_text_color(rec_status_lbl, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_font(rec_status_lbl, &lv_font_montserrat_20, 0);

    // Large centered elapsed time — fixed font so it never outgrows the page.
    rec_time_lbl = lv_label_create(main_page);
    lv_label_set_text(rec_time_lbl, "00:00");
    lv_obj_set_style_text_color(rec_time_lbl, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(rec_time_lbl, &lv_font_montserrat_48, 0);

    // Progress bar: fraction of HW_REC_MAX_MS used.
    rec_bar = lv_bar_create(main_page);
    lv_obj_set_width(rec_bar, LV_PCT(80));
    lv_obj_set_height(rec_bar, 6);
    lv_bar_set_range(rec_bar, 0, (int32_t)(HW_REC_MAX_MS / 1000));
    lv_bar_set_value(rec_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(rec_bar, UI_COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rec_bar, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_radius(rec_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(rec_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rec_bar, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(rec_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(rec_bar, 3, LV_PART_INDICATOR);

    // Secondary line: cap and bytes written.
    rec_size_lbl = lv_label_create(main_page);
    char ibuf[48];
    snprintf(ibuf, sizeof(ibuf), "max %u:%02u  -  0 KB",
             (unsigned)(HW_REC_MAX_MS / 60000),
             (unsigned)((HW_REC_MAX_MS / 1000) % 60));
    lv_label_set_text(rec_size_lbl, ibuf);
    lv_obj_set_style_text_color(rec_size_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(rec_size_lbl, get_small_font(), 0);

    // Prominent red stop button.
    lv_obj_t *stop_btn = lv_btn_create(main_page);
    lv_obj_set_width(stop_btn, LV_PCT(80));
    lv_obj_set_height(stop_btn, 44);
    lv_obj_set_style_bg_color(stop_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_radius(stop_btn, UI_RADIUS, 0);
    lv_obj_set_style_shadow_color(stop_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_shadow_width(stop_btn, 18, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_opa(stop_btn, LV_OPA_40, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(stop_btn, stop_rec_and_return, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, LV_SYMBOL_STOP "  Stop");
    lv_obj_set_style_text_color(stop_lbl, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(stop_lbl);
    lv_group_add_obj(lv_group_get_default(), stop_btn);
    lv_group_focus_obj(stop_btn);

    if (!tick_timer) {
        tick_timer = lv_timer_create(rec_tick_cb, 200, NULL);
    }
}

// --- playback view -----------------------------------------------------

static lv_obj_t *pb_status_lbl = NULL;
static bool      pb_was_playing = false;

static void pb_tick_cb(lv_timer_t *t)
{
    if (!pb_status_lbl) return;
    bool running = hw_player_running();
    if (pb_was_playing && !running) {
        lv_label_set_text(pb_status_lbl, "Ended");
    }
    pb_was_playing = running;
}

static void pb_play_cb(lv_event_t *e)
{
    if (selected_path.empty()) return;
    // Filename without leading '/', matching hw_set_sd_music_play's convention.
    const char *fn = selected_path.c_str();
    if (*fn == '/') ++fn;
    hw_set_sd_music_play(AUDIO_SOURCE_SDCARD, fn);
    if (pb_status_lbl) lv_label_set_text(pb_status_lbl, LV_SYMBOL_PLAY "  Playing...");
    pb_was_playing = true;
    if (!tick_timer) {
        tick_timer = lv_timer_create(pb_tick_cb, 250, NULL);
    }
}

static void pb_stop_cb(lv_event_t *e)
{
    hw_set_play_stop();
    if (pb_status_lbl) lv_label_set_text(pb_status_lbl, "Stopped");
    pb_was_playing = false;
}

static lv_obj_t *pb_vol_slider = NULL;
static lv_obj_t *pb_vol_pct_lbl = NULL;

static void pb_update_vol_label(int v)
{
    if (!pb_vol_pct_lbl) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(pb_vol_pct_lbl, buf);
}

static void pb_volume_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    hw_set_volume((uint8_t)v);
    pb_update_vol_label(v);
}

static void pb_delete_cb(lv_event_t *e)
{
    hw_set_play_stop();
    if (!selected_path.empty()) {
        hw_delete_file(selected_path.c_str());
    }
    selected_path.clear();
    reload_notes();
    enter_list();
}

static void pb_back_cb(lv_event_t *e)
{
    hw_set_play_stop();
    enter_list();
}

static void build_playback_view()
{
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(main_page, 12, 0);
    lv_obj_set_style_pad_row(main_page, 8, 0);

    lv_obj_t *name_lbl = lv_label_create(main_page);
    lv_label_set_text(name_lbl, selected_name.c_str());
    lv_obj_set_width(name_lbl, LV_PCT(100));
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name_lbl, UI_COLOR_FG, 0);

    lv_obj_t *meta_lbl = lv_label_create(main_page);
    std::string meta = format_duration_s(bytes_to_seconds(selected_bytes))
                     + "   " + format_date(selected_mtime);
    lv_label_set_text(meta_lbl, meta.c_str());
    lv_obj_set_style_text_color(meta_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(meta_lbl, get_small_font(), 0);

    pb_status_lbl = lv_label_create(main_page);
    lv_label_set_text(pb_status_lbl, "Ready");
    lv_obj_set_style_text_color(pb_status_lbl, UI_COLOR_ACCENT, 0);

    // Volume row: icon + slider.
    lv_obj_t *vol_row = lv_obj_create(main_page);
    lv_obj_set_width(vol_row, LV_PCT(100));
    lv_obj_set_height(vol_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_set_style_pad_column(vol_row, 10, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *vol_icon = lv_label_create(vol_row);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(vol_icon, UI_COLOR_FG, 0);

    uint8_t cur_vol = hw_get_volume();

    pb_vol_slider = lv_slider_create(vol_row);
    lv_obj_set_flex_grow(pb_vol_slider, 1);
    lv_slider_set_range(pb_vol_slider, 0, 100);
    lv_slider_set_value(pb_vol_slider, cur_vol, LV_ANIM_OFF);
    // Tall, high-contrast bar so up/down changes are obvious.
    lv_obj_set_style_height(pb_vol_slider, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pb_vol_slider, UI_COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pb_vol_slider, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(pb_vol_slider, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pb_vol_slider, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(pb_vol_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(pb_vol_slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_size(pb_vol_slider, 20, 20, LV_PART_KNOB);
    lv_obj_set_style_bg_color(pb_vol_slider, UI_COLOR_FG, LV_PART_KNOB);
    lv_obj_set_style_radius(pb_vol_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    // Highlight the whole bar when focused so the user sees they're in it.
    lv_obj_set_style_outline_width(pb_vol_slider, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(pb_vol_slider, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(pb_vol_slider, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(pb_vol_slider, pb_volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(lv_group_get_default(), pb_vol_slider);

    pb_vol_pct_lbl = lv_label_create(vol_row);
    lv_obj_set_style_text_color(pb_vol_pct_lbl, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(pb_vol_pct_lbl, get_small_font(), 0);
    lv_obj_set_style_min_width(pb_vol_pct_lbl, 40, 0);
    lv_obj_set_style_text_align(pb_vol_pct_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    pb_update_vol_label(cur_vol);

    // Button row.
    lv_obj_t *row = lv_obj_create(main_page);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto make_btn = [&](const char *icon, const char *txt, lv_event_cb_t cb,
                        lv_color_t bg) -> lv_obj_t * {
        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_radius(btn, UI_RADIUS, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "%s %s", icon, txt);
        lv_obj_set_style_text_color(lbl, UI_COLOR_FG, 0);
        lv_obj_center(lbl);
        lv_group_add_obj(lv_group_get_default(), btn);
        return btn;
    };

    lv_obj_t *play_btn = make_btn(LV_SYMBOL_PLAY, "Play", pb_play_cb, UI_COLOR_ACCENT);
    make_btn(LV_SYMBOL_STOP, "Stop", pb_stop_cb, lv_palette_main(LV_PALETTE_GREY));
    make_btn(LV_SYMBOL_TRASH, "Del",  pb_delete_cb, lv_palette_main(LV_PALETTE_RED));
    make_btn(LV_SYMBOL_LEFT, "Back",  pb_back_cb, lv_palette_main(LV_PALETTE_GREY));

    lv_group_focus_obj(play_btn);
}

// --- view transitions --------------------------------------------------

static void kill_tick_timer()
{
    if (tick_timer) {
        lv_timer_del(tick_timer);
        tick_timer = NULL;
    }
}

static void clear_view_refs()
{
    rec_time_lbl = NULL;
    rec_status_lbl = NULL;
    rec_size_lbl = NULL;
    rec_dot = NULL;
    rec_bar = NULL;
    pb_status_lbl = NULL;
    pb_vol_slider = NULL;
    pb_vol_pct_lbl = NULL;
}

static void render_view()
{
    if (!main_page) return;
    lv_obj_clean(main_page);
    clear_view_refs();
    kill_tick_timer();

    switch (current_view) {
    case VIEW_MAIN:     build_main_view(); break;
    case VIEW_LIST:     build_list_view(); break;
    case VIEW_RECORD:   build_record_view(); break;
    case VIEW_PLAYBACK: build_playback_view(); break;
    }
}

static void enter_main()
{
    current_view = VIEW_MAIN;
    render_view();
}

static void enter_list()
{
    reload_notes();
    current_view = VIEW_LIST;
    render_view();
}

static void enter_record()
{
    if (!hw_mic_available()) return;
    if (hw_player_running()) hw_set_play_stop();
    if (hw_rec_running()) return;

    pending_rec_path = build_new_note_path();
    if (!hw_rec_start(pending_rec_path.c_str())) {
        // Silent failure — remain on list view. Could show a toast here.
        return;
    }
    current_view = VIEW_RECORD;
    render_view();
}

static void enter_playback(const Note &n)
{
    selected_path   = n.full_path;
    selected_name   = n.filename;
    selected_bytes  = n.data_bytes;
    selected_mtime  = n.mtime;
    current_view    = VIEW_PLAYBACK;
    render_view();
}

// --- lifecycle ---------------------------------------------------------

// In quick-record (home shortcut) every back press exits the app. In the
// browse-only flow (Settings → Recordings) the list view is the root, so
// VIEW_PLAYBACK pops back to it and VIEW_LIST exits.
static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        if (!s_quick_record && current_view == VIEW_PLAYBACK) {
            hw_set_play_stop();
            enter_list();
            return;
        }
        exit_to_menu();
    }
}

static void exit_btn_cb(lv_event_t *e)
{
    if (!s_quick_record && current_view == VIEW_PLAYBACK) {
        hw_set_play_stop();
        enter_list();
        return;
    }
    exit_to_menu();
}

static void ui_notes_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    ensure_notes_dir();
    selected_path.clear();
    selected_name.clear();
    selected_bytes = 0;
    selected_mtime = 0;

    // Pick the initial view.
    //   - Quick-record (home shortcut) jumps straight into a fresh recording
    //     when the mic is available; falls back to the browse list if the
    //     mic is missing or hw_rec_start fails, so the user sees something
    //     instead of a silent failure.
    //   - Otherwise (Settings → Recordings) opens directly on the list.
    if (s_quick_record && hw_mic_available()) {
        if (hw_player_running()) hw_set_play_stop();
        pending_rec_path = build_new_note_path();
        if (hw_rec_start(pending_rec_path.c_str())) {
            current_view = VIEW_RECORD;
        } else {
            s_quick_record = false;
            reload_notes();
            current_view = VIEW_LIST;
        }
    } else {
        if (s_quick_record) s_quick_record = false;  // no mic — drop quick mode
        reload_notes();
        current_view = VIEW_LIST;
    }

    menu = create_menu(parent, back_event_handler);
    main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_all(main_page, 0, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    ui_show_back_button(exit_btn_cb);

    lv_menu_set_page(menu, main_page);

    render_view();

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        if (!s_quick_record && current_view == VIEW_PLAYBACK) {
            hw_set_play_stop();
            enter_list();
            return;
        }
        exit_to_menu();
    }, NULL);
#endif
}

static void ui_notes_exit(lv_obj_t *parent)
{
    ui_hide_back_button();
    disable_keyboard();
    kill_tick_timer();
    if (hw_rec_running()) hw_rec_stop();
    hw_set_play_stop();
    if (menu) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
    }
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    main_page = NULL;
    clear_view_refs();
    notes.clear();
    selected_path.clear();
    selected_name.clear();
}

} // namespace

namespace apps {
class AudioNotesApp : public core::App {
public:
    AudioNotesApp(const char *name, bool quick_record)
        : core::App(name), m_quick_record(quick_record) {}
    void onStart(lv_obj_t *parent) override {
        s_quick_record = m_quick_record;
        ui_notes_enter(parent);
    }
    void onStop() override {
        ui_notes_exit(getRoot());
        core::App::onStop();
    }
private:
    bool m_quick_record;
};

std::shared_ptr<core::App> make_audio_notes_app() {
    // Home-screen shortcut — jumps straight into recording.
    return std::make_shared<AudioNotesApp>("Recorder", true);
}
std::shared_ptr<core::App> make_audio_recordings_app() {
    // Settings → Recordings — opens the Record/Browse menu.
    return std::make_shared<AudioNotesApp>("Recordings", false);
}
} // namespace apps
