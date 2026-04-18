/**
 * @file      ui_audio_notes.cpp
 * @brief     Audio "mental notes" app — record voice memos to SD, play them
 *            back, delete. All files live in /mental_notes/ on the SD card.
 */
#include "ui_define.h"
#include "core/app_manager.h"
#include "hal/system.h"
#include "hal/storage.h"
#include "hal/audio.h"
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

static ViewMode current_view = VIEW_LIST;
static std::vector<Note> notes;
static std::string selected_path;   // for playback view
static std::string selected_name;
static uint32_t    selected_bytes = 0;
static uint32_t    selected_mtime = 0;
static std::string pending_rec_path; // set while recording

static void ui_notes_enter(lv_obj_t *parent);
static void ui_notes_exit(lv_obj_t *parent);
static void render_view();
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
    instance.lockSPI();
    if (!SD.exists(NOTES_DIR)) SD.mkdir(NOTES_DIR);
    instance.unlockSPI();
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
        instance.lockSPI();
        File f = SD.open(n.full_path.c_str());
        if (f) {
            size_t sz = f.size();
            n.data_bytes = (sz >= 44) ? (uint32_t)(sz - 44) : 0;
            f.close();
        }
        instance.unlockSPI();
#endif
        notes.push_back(n);
    }
    // Sort newest first by mtime, tie-break on filename desc.
    std::sort(notes.begin(), notes.end(), [](const Note &a, const Note &b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return a.filename > b.filename;
    });
}

// --- list view ---------------------------------------------------------

static void record_btn_cb(lv_event_t *e) { enter_record(); }

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

    // Record button.
    lv_obj_t *rec_btn = lv_btn_create(main_page);
    lv_obj_set_width(rec_btn, LV_PCT(100));
    lv_obj_set_height(rec_btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(rec_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_radius(rec_btn, UI_RADIUS, 0);
    lv_obj_add_event_cb(rec_btn, record_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rec_lbl = lv_label_create(rec_btn);
    lv_label_set_text(rec_lbl, LV_SYMBOL_AUDIO "  Record new note");
    lv_obj_set_style_text_color(rec_lbl, UI_COLOR_FG, 0);
    lv_obj_center(rec_lbl);
    lv_group_add_obj(lv_group_get_default(), rec_btn);

    bool mic_ok = hw_mic_available();
    if (!mic_ok) {
        lv_obj_add_state(rec_btn, LV_STATE_DISABLED);
        lv_label_set_text(rec_lbl, LV_SYMBOL_WARNING "  No microphone");
    }

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
        if (lv_obj_get_child_count(list) > 0) {
            lv_group_focus_obj(lv_obj_get_child(list, 0));
        }
    }

    if (!mic_ok && notes.empty()) {
        // Nothing to focus — fine.
    } else if (mic_ok && notes.empty()) {
        lv_group_focus_obj(rec_btn);
    }
}

// --- record view -------------------------------------------------------

static lv_obj_t *rec_time_lbl = NULL;
static lv_obj_t *rec_status_lbl = NULL;
static lv_obj_t *rec_size_lbl = NULL;

static void stop_rec_and_return(lv_event_t *e)
{
    if (hw_rec_running()) {
        hw_rec_stop();
    }
    reload_notes();
    enter_list();
}

static void rec_tick_cb(lv_timer_t *t)
{
    if (!rec_time_lbl) return;
    uint32_t ms = hw_rec_elapsed_ms();
    uint32_t secs = ms / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02u:%02u / %02u:%02u",
             (unsigned)(secs / 60), (unsigned)(secs % 60),
             (unsigned)(HW_REC_MAX_MS / 60000),
             (unsigned)((HW_REC_MAX_MS / 1000) % 60));
    lv_label_set_text(rec_time_lbl, buf);

    if (rec_size_lbl) {
        uint32_t bytes = hw_rec_bytes_written();
        char sbuf[32];
        snprintf(sbuf, sizeof(sbuf), "%u KB", (unsigned)(bytes / 1024));
        lv_label_set_text(rec_size_lbl, sbuf);
    }

    if (!hw_rec_running()) {
        // Auto-stop (cap hit) — return to list.
        reload_notes();
        enter_list();
    }
}

static void build_record_view()
{
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main_page, 16, 0);
    lv_obj_set_style_pad_row(main_page, 12, 0);

    rec_status_lbl = lv_label_create(main_page);
    lv_label_set_text(rec_status_lbl, LV_SYMBOL_AUDIO "  Recording");
    lv_obj_set_style_text_color(rec_status_lbl, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_font(rec_status_lbl, get_header_font(), 0);

    rec_time_lbl = lv_label_create(main_page);
    lv_label_set_text(rec_time_lbl, "00:00 / 05:00");
    lv_obj_set_style_text_color(rec_time_lbl, UI_COLOR_FG, 0);

    rec_size_lbl = lv_label_create(main_page);
    lv_label_set_text(rec_size_lbl, "0 KB");
    lv_obj_set_style_text_color(rec_size_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(rec_size_lbl, get_small_font(), 0);

    lv_obj_t *stop_btn = lv_btn_create(main_page);
    lv_obj_set_style_bg_color(stop_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(stop_btn, UI_RADIUS, 0);
    lv_obj_add_event_cb(stop_btn, stop_rec_and_return, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, LV_SYMBOL_STOP "  Stop");
    lv_obj_set_style_text_color(stop_lbl, UI_COLOR_FG, 0);
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
    case VIEW_LIST:     build_list_view(); break;
    case VIEW_RECORD:   build_record_view(); break;
    case VIEW_PLAYBACK: build_playback_view(); break;
    }
}

static void enter_list()
{
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

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        ui_notes_exit(NULL);
        menu_show();
    }
}

static void exit_btn_cb(lv_event_t *e)
{
    if (hw_rec_running()) hw_rec_stop();
    hw_set_play_stop();
    ui_notes_exit(NULL);
    menu_show();
}

static void ui_notes_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    ensure_notes_dir();
    current_view = VIEW_LIST;
    selected_path.clear();
    selected_name.clear();
    selected_bytes = 0;
    selected_mtime = 0;

    reload_notes();

    menu = create_menu(parent, back_event_handler);
    main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_all(main_page, 0, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    ui_show_back_button(exit_btn_cb);

    render_view();

    lv_menu_set_page(menu, main_page);

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        if (hw_rec_running()) hw_rec_stop();
        hw_set_play_stop();
        ui_notes_exit(NULL);
        menu_show();
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
    AudioNotesApp() : core::App("Notes") {}
    void onStart(lv_obj_t *parent) override { ui_notes_enter(parent); }
    void onStop() override {
        ui_notes_exit(getRoot());
        core::App::onStop();
    }
};

std::shared_ptr<core::App> make_audio_notes_app() {
    return std::make_shared<AudioNotesApp>();
}
} // namespace apps
