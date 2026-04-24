/**
 * @file      ui_journal.cpp
 * @brief     Chronological view of internal files with timestamps in blog style.
 */
#include "../ui_define.h"
#include "app_registry.h"
#include "../core/app_manager.h"
#include "../hal/notes_crypto.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace {

struct JournalEntry {
    std::string filename;
    uint32_t mtime;
    std::string snippet;
    bool truncated;
};

constexpr const char *kIndexPath = "/journal_idx.bin";
constexpr uint32_t kIndexMagic = 0x4A524E4Cu; // 'JRNL' little-endian
constexpr uint8_t  kIndexVersion = 1;
constexpr size_t   kSnippetBytes = 4096;
#define JOURNAL_PAGE_SIZE 5

void write_u16le(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
}

void write_u32le(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
    out.push_back((uint8_t)((v >> 16) & 0xFF));
    out.push_back((uint8_t)((v >> 24) & 0xFF));
}

bool read_u16le(const std::vector<uint8_t> &in, size_t &pos, uint16_t &v)
{
    if (pos + 2 > in.size()) return false;
    v = (uint16_t)in[pos] | ((uint16_t)in[pos + 1] << 8);
    pos += 2;
    return true;
}

bool read_u32le(const std::vector<uint8_t> &in, size_t &pos, uint32_t &v)
{
    if (pos + 4 > in.size()) return false;
    v = (uint32_t)in[pos] |
        ((uint32_t)in[pos + 1] << 8) |
        ((uint32_t)in[pos + 2] << 16) |
        ((uint32_t)in[pos + 3] << 24);
    pos += 4;
    return true;
}

bool load_index_file(std::vector<JournalEntry> &out)
{
    out.clear();
    std::vector<uint8_t> raw;
    if (!hw_read_preferred_bytes(kIndexPath, raw)) return false;

    /* Encrypted indices carry the OpenSSL Salted__ prefix. Decrypt in place
     * before the header checks below try to interpret the bytes. */
    if (raw.size() >= 8 && memcmp(raw.data(), "Salted__", 8) == 0) {
        if (!notes_crypto_is_unlocked()) return false;
        std::string plain;
        if (!notes_crypto_decrypt_buffer(raw.data(), raw.size(), plain)) return false;
        raw.assign(plain.begin(), plain.end());
    }

    if (raw.size() < 9) return false;

    size_t pos = 0;
    uint32_t magic;
    if (!read_u32le(raw, pos, magic) || magic != kIndexMagic) return false;
    if (raw[pos++] != kIndexVersion) return false;
    uint32_t count;
    if (!read_u32le(raw, pos, count)) return false;

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        JournalEntry e;
        uint32_t mtime;
        if (!read_u32le(raw, pos, mtime)) return false;
        e.mtime = mtime;

        if (pos >= raw.size()) return false;
        uint8_t name_len = raw[pos++];
        if (pos + name_len > raw.size()) return false;
        e.filename.assign((const char *)&raw[pos], name_len);
        pos += name_len;

        uint16_t snip_len;
        if (!read_u16le(raw, pos, snip_len)) return false;
        if (pos + snip_len + 1 > raw.size()) return false;
        e.snippet.assign((const char *)&raw[pos], snip_len);
        pos += snip_len;
        e.truncated = (raw[pos++] != 0);

        out.push_back(std::move(e));
    }
    return true;
}

void save_index_file(const std::vector<JournalEntry> &entries)
{
    std::vector<uint8_t> buf;
    size_t est_size = 16;
    for (const auto &e : entries) {
        est_size += 8 + e.filename.size() + e.snippet.size();
    }
    buf.reserve(est_size);

    write_u32le(buf, kIndexMagic);
    buf.push_back(kIndexVersion);
    write_u32le(buf, (uint32_t)entries.size());
    for (const auto &e : entries) {
        write_u32le(buf, e.mtime);
        uint8_t name_len = (uint8_t)std::min<size_t>(e.filename.size(), 255);
        buf.push_back(name_len);
        buf.insert(buf.end(), e.filename.begin(), e.filename.begin() + name_len);
        uint16_t snip_len = (uint16_t)std::min<size_t>(e.snippet.size(), 65535);
        write_u16le(buf, snip_len);
        buf.insert(buf.end(), e.snippet.begin(), e.snippet.begin() + snip_len);
        buf.push_back(e.truncated ? 1 : 0);
    }

    /* The index caches the first 4KB of every note as a plaintext snippet.
     * Without encryption that would defeat the at-rest protection on the
     * notes themselves, so we encrypt the whole blob with the same key. */
    if (notes_crypto_should_encrypt()) {
        std::vector<uint8_t> ct;
        if (notes_crypto_encrypt_buffer(buf.data(), buf.size(), ct)) {
            hw_save_preferred_bytes(kIndexPath, ct.data(), ct.size());
            return;
        }
        /* Encryption failure falls through to the plaintext write so we don't
         * lose the index entirely on transient errors. */
    }
    hw_save_preferred_bytes(kIndexPath, buf.data(), buf.size());
}

using ProgressFn = void (*)(void *ud, const char *phase, int cur, int total, const char *detail);

static void report(ProgressFn cb, void *ud, const char *phase, int cur, int total, const char *detail)
{
    if (cb) {
        static uint32_t last_refr = 0;
        uint32_t now = lv_tick_get();
        cb(ud, phase, cur, total, detail);
        if (lv_tick_elaps(last_refr) > 100) {
            lv_refr_now(NULL);
            last_refr = now;
            // The scan/decrypt loop runs inside lv_timer_handler on the
            // dedicated LVGL task. Without a yield here, large journals keep
            // IDLE1 off the CPU long enough for the task watchdog to panic the
            // device. vTaskDelay(1) costs one tick but guarantees IDLE runs.
#ifdef ARDUINO
            vTaskDelay(1);
#endif
        }
    }
}

void refresh_journal_entries(std::vector<JournalEntry> &entries, ProgressFn cb = nullptr, void *ud = nullptr)
{
    static ProgressFn g_cb = nullptr;
    static void *g_ud = nullptr;
    g_cb = cb;
    g_ud = ud;

    report(cb, ud, "Scanning files...", 0, 0, nullptr);
    std::vector<std::pair<std::string, uint32_t>> current;
    hw_get_preferred_txt_files_info(current, [](int cur, int total, const char *name) {
        if (g_cb) g_cb(g_ud, "Scanning files...", cur, 0, name);
    });

    current.erase(std::remove_if(current.begin(), current.end(),
                                 [](const std::pair<std::string, uint32_t> &p) {
                                     return p.first == "tasks.txt" ||
                                            p.first == "/tasks.txt";
                                 }),
                  current.end());

    std::sort(current.begin(), current.end(), [](const std::pair<std::string, uint32_t> &a,
                                                  const std::pair<std::string, uint32_t> &b) {
        return a.first > b.first;
    });

    report(cb, ud, "Scanning files...", (int)current.size(), (int)current.size(), nullptr);

    std::vector<JournalEntry> existing;
    if (entries.empty()) {
        report(cb, ud, "Loading index...", 0, 1, nullptr);
        load_index_file(existing);
        report(cb, ud, "Loading index...", 1, 1, nullptr);
    } else {
        existing = std::move(entries);
        entries.clear();
    }

    std::unordered_map<std::string, size_t> existing_map;
    for (size_t i = 0; i < existing.size(); ++i) {
        existing_map[existing[i].filename] = i;
    }

    std::vector<JournalEntry> next;
    next.reserve(current.size());
    bool dirty = (existing.size() != current.size());
    int total = (int)current.size();
    int done = 0;

    for (const auto &cur : current) {
        const std::string &name = cur.first;
        uint32_t mtime = cur.second;

        auto it = existing_map.find(name);
        bool needs_rebuild = true;
        
        if (it != existing_map.end()) {
            const JournalEntry &old_e = existing[it->second];
            if (old_e.mtime == mtime) {
                next.push_back(std::move(existing[it->second]));
                needs_rebuild = false;
            }
        }

        if (needs_rebuild) {
            report(cb, ud, "Reading previews...", done, total, name.c_str());
            JournalEntry e;
            e.filename = name;
            e.mtime = mtime;
            std::string snippet;
            bool truncated = false;
            hw_read_preferred_file_snippet(name.c_str(), snippet, kSnippetBytes, &truncated);
            e.snippet = std::move(snippet);
            e.truncated = truncated;
            next.push_back(std::move(e));
            dirty = true;
        }
        done++;
        if (total > 0 && cb) {
            report(cb, ud, needs_rebuild ? "Reading previews..." : "Processing...", done, total, name.c_str());
        }
    }

    std::sort(next.begin(), next.end(), [](const JournalEntry &a, const JournalEntry &b) {
        return a.filename > b.filename;
    });

    if (dirty) {
        report(cb, ud, "Saving index...", 0, 1, nullptr);
        save_index_file(next);
        report(cb, ud, "Saving index...", 1, 1, nullptr);
    }

    entries = std::move(next);
}

static lv_obj_t *menu = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *parent_obj = NULL;
static int target_focus_index = -1;
static int current_page = 0;
static std::vector<JournalEntry> journal_entries;
static bool cache_valid = false;

static ui_loading_t loader = {};
static uint32_t loader_started_ms = 0;

static void loader_show()
{
    ui_loading_open(&loader, "Loading Journal", "Starting...");
    loader_started_ms = lv_tick_get();
}

// Loader callback. Bridges the scan's (phase, cur/total, detail) triplet to
// the unified popup: the phase string becomes the detail line when there's
// no file name to show, and the progress bar tracks cur/total.
static void loader_update(void *ud, const char *phase, int cur, int total, const char *detail)
{
    (void)ud;
    if (!loader.overlay) return;

    // Compose a two-line detail: phase on top, current filename below.
    // Elapsed ms is appended to phase so debugging info stays visible
    // without adding another label. Long filenames are DOT-trimmed by
    // the shared detail label.
    char line[96];
    uint32_t elapsed = lv_tick_elaps(loader_started_ms);
    if (detail && detail[0]) {
        snprintf(line, sizeof(line), "%s (%lums)\n%s",
                 phase ? phase : "", (unsigned long)elapsed, detail);
    } else {
        snprintf(line, sizeof(line), "%s (%lums)",
                 phase ? phase : "", (unsigned long)elapsed);
    }
    ui_loading_set_progress(&loader, cur, total, line);
}

static void loader_hide()
{
    ui_loading_close(&loader);
}

void ui_journal_enter(lv_obj_t *parent);
void ui_journal_exit(lv_obj_t *parent);
static void journal_build_ui(lv_obj_t *parent);

static lv_obj_t *pending_journal_parent = NULL;
static void journal_unlock_result_cb(bool ok, void *ud)
{
    lv_obj_t *parent = (lv_obj_t *)ud;
    if (!ok) {
        pending_journal_parent = NULL;
        menu_show();
        return;
    }
    if (pending_journal_parent == parent) {
        pending_journal_parent = NULL;
        journal_build_ui(parent);
    }
}

static void free_post_user_data()
{
    if (!menu) return;
    lv_obj_t *main_page = lv_menu_get_cur_main_page(menu);
    if (!main_page) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(main_page); i++) {
        lv_obj_t *child = lv_obj_get_child(main_page, i);
        void *data = lv_obj_get_user_data(child);
        if (data) {
            lv_mem_free(data);
            lv_obj_set_user_data(child, NULL);
        }
    }
}

static void teardown_menu_for_rerender()
{
    free_post_user_data();
    lv_obj_clean(menu);
    lv_obj_del(menu);
    menu = NULL;
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
}

static void do_exit()
{
    ui_journal_exit(NULL);
    menu_show();
}

static void next_page_cb(lv_event_t *e)
{
    current_page++;
    teardown_menu_for_rerender();
    ui_journal_enter(parent_obj);
}

static void prev_page_cb(lv_event_t *e)
{
    if (current_page > 0) {
        current_page--;
        teardown_menu_for_rerender();
        ui_journal_enter(parent_obj);
    }
}

static void back_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (lv_menu_back_btn_is_root(menu, obj)) {
        do_exit();
    }
}

static void exit_btn_cb(lv_event_t *e)
{
    do_exit();
}

static void post_focus_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    lv_obj_scroll_to_view(obj, LV_ANIM_ON);
}

static void post_defocus_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    lv_group_t *g = lv_group_get_default();
    if (!lv_group_get_editing(g)) return;
    lv_group_set_editing(g, false);
    lv_obj_t *scroll_area = lv_obj_get_child(obj, 0);
    if (scroll_area) {
        lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_scroll_to_y(scroll_area, 0, LV_ANIM_OFF);
    }
}

static void post_click_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    lv_group_t *g = lv_group_get_default();
    lv_obj_t *scroll_area = lv_obj_get_child(obj, 0);
    
    if (lv_group_get_editing(g) && lv_group_get_focused(g) == obj) {
        lv_group_set_editing(g, false);
        if (scroll_area) {
            lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_scroll_to_y(scroll_area, 0, LV_ANIM_ON);
        }
    } else {
        lv_group_set_editing(g, true);
        if (scroll_area) lv_obj_add_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void delete_msgbox_cb(lv_event_t *e)
{
    uint16_t id = 0;
    lv_obj_t *mbox_to_del = NULL;
#if LVGL_VERSION_MAJOR == 9
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *text = lv_label_get_text(label);
    if (strcmp(text, "No") == 0) {
        id = 1;
    }
    mbox_to_del = lv_obj_get_parent(lv_obj_get_parent(btn));
#else
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    id = lv_msgbox_get_active_btn(obj);
    mbox_to_del = obj;
#endif

    const char *path = (const char *)lv_event_get_user_data(e);
    bool deleted = false;

    if (id == 0) { // Yes
        if (hw_delete_preferred_file(path)) {
            journal_entries.erase(std::remove_if(journal_entries.begin(), journal_entries.end(),
                                              [&](const JournalEntry &be) { return be.filename == path; }),
                               journal_entries.end());
            save_index_file(journal_entries);
            cache_valid = true;
            hw_set_filesystem_dirty(false);
            deleted = true;
        }
    } else {
        target_focus_index = -1;
    }

    destroy_msgbox(mbox_to_del);

    if (deleted) {
        teardown_menu_for_rerender();
        ui_journal_enter(parent_obj);
    }

    if (path) lv_mem_free((void *)path);
}

static void show_delete_confirm(const char *filename)
{
    static const char *btns[] = {"Yes", "No", ""};
    char msg[128];
    snprintf(msg, sizeof(msg), "Delete this entry?\n%s", filename);

    char *path_dup = (char *)lv_mem_alloc(strlen(filename) + 1);
    strcpy(path_dup, filename);

    create_msgbox(NULL, "Confirm", msg, btns, delete_msgbox_cb, path_dup);
}

static void journal_key_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_KEY) return;
    uint32_t key = lv_event_get_key(e);
    if (key == 'r' || key == 'R') {
        cache_valid = false;
        ui_journal_exit(NULL);
        ui_journal_enter(parent_obj);
        return;
    }
    if (key != 'd' && key != 'D' && key != LV_KEY_BACKSPACE) return;

    lv_group_t *g = lv_group_get_default();
    lv_obj_t *focused = lv_group_get_focused(g);
    if (!focused) return;
    const char *filename = (const char *)lv_obj_get_user_data(focused);
    if (!filename) return;

    lv_obj_t *page = lv_menu_get_cur_main_page(menu);
    if (page) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(page); i++) {
            if (lv_obj_get_child(page, i) == focused) {
                target_focus_index = (int)i;
                break;
            }
        }
    }
    show_delete_confirm(filename);
}

static std::string parse_filename_to_human(const std::string &filename)
{
    size_t start = (filename[0] == '/') ? 1 : 0;
    if (filename.length() < start + 15) return filename;

    std::string y = filename.substr(start, 4);
    std::string m = filename.substr(start + 4, 2);
    std::string d = filename.substr(start + 6, 2);
    std::string hh = filename.substr(start + 9, 2);
    std::string mm = filename.substr(start + 11, 2);
    std::string ss = filename.substr(start + 13, 2);

    const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int month_idx = atoi(m.c_str()) - 1;
    if (month_idx < 0 || month_idx > 11) return filename;

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s %s, %s - %s:%s:%s",
             months[month_idx], d.c_str(), y.c_str(), hh.c_str(), mm.c_str(), ss.c_str());
    return std::string(buffer);
}

static void post_scroll_indicator_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *indicator = (lv_obj_t *)lv_event_get_user_data(e);
    if (!indicator) return;
    
    if (lv_obj_get_scroll_bottom(obj) <= 0) {
        lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_journal_enter(lv_obj_t *parent)
{
    if (notes_crypto_is_enabled() && !notes_crypto_is_unlocked()) {
        pending_journal_parent = parent;
        ui_passphrase_unlock(journal_unlock_result_cb, parent);
        return;
    }
    journal_build_ui(parent);
}

static void journal_build_ui(lv_obj_t *parent)
{
    if (menu != NULL) return;
    enable_keyboard();
    parent_obj = parent;

    if (hw_get_filesystem_dirty()) {
        cache_valid = false;
        hw_set_filesystem_dirty(false);
    }

    bool show_loader = !cache_valid || journal_entries.empty();
    if (show_loader) loader_show();
    menu = create_menu(parent, back_event_handler);
    if (show_loader) {
        refresh_journal_entries(journal_entries, loader_update, nullptr);
        cache_valid = true;
    }

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_all(main_page, 0, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(main_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(main_page, journal_key_event_cb, LV_EVENT_KEY, NULL);

    // Back button moves to the top status bar. The journal-specific focus and
    // key handlers stay attached to it.
    lv_obj_t *exit_btn = ui_show_back_button(exit_btn_cb);
    if (exit_btn) {
        lv_obj_add_event_cb(exit_btn, post_focus_cb, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(exit_btn, journal_key_event_cb, LV_EVENT_KEY, NULL);
    }

    int total_files = (int)journal_entries.size();
    int start_idx = current_page * JOURNAL_PAGE_SIZE;
    int end_idx = std::min(start_idx + JOURNAL_PAGE_SIZE, total_files);

    if (start_idx >= total_files && total_files > 0) {
        current_page = (total_files - 1) / JOURNAL_PAGE_SIZE;
        start_idx = current_page * JOURNAL_PAGE_SIZE;
        end_idx = total_files;
    }

    lv_obj_t *focus_target = exit_btn;
    // The back button no longer occupies main_page's child 0, so child indices
    // for prev_btn_cont and post_conts start at 0 instead of 1.
    int current_child_idx = -1;
    std::vector<std::pair<lv_obj_t *, lv_obj_t *>> trunc_checks;

    if (total_files == 0) {
        lv_obj_t *empty_label = lv_label_create(main_page);
        lv_label_set_text(empty_label, "No entries found.");
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty_label, lv_pct(100));
    } else {
        const lv_font_t *font = get_journal_font();

        if (current_page > 0) {
            lv_obj_t *prev_cont = lv_menu_cont_create(main_page);
            lv_obj_t *prev_btn = lv_btn_create(prev_cont);
            lv_obj_t *prev_label = lv_label_create(prev_btn);
            lv_label_set_text(prev_label, LV_SYMBOL_UP " Previous Page");
            lv_obj_add_event_cb(prev_btn, prev_page_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), prev_btn);
            lv_obj_add_event_cb(prev_btn, post_focus_cb, LV_EVENT_FOCUSED, NULL);
            current_child_idx++;
        }

        int render_total = end_idx - start_idx;
        trunc_checks.reserve(render_total);
        for (int i = start_idx; i < end_idx; ++i) {
            const JournalEntry &entry = journal_entries[i];
            current_child_idx++;
            loader_update(nullptr, "Rendering entries...",
                          i - start_idx, render_total, entry.filename.c_str());

            lv_obj_t *post_cont = lv_obj_create(main_page);
            lv_obj_set_width(post_cont, lv_pct(100));
            lv_obj_set_height(post_cont, LV_SIZE_CONTENT);
            lv_obj_set_style_max_height(post_cont, 130, 0);
            lv_obj_set_flex_flow(post_cont, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(post_cont, 0, 0); 
            lv_obj_set_style_pad_gap(post_cont, 0, 0);
            lv_obj_set_style_border_width(post_cont, 0, 0); 
            lv_obj_set_style_bg_opa(post_cont, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(post_cont, lv_color_black(), 0);
            lv_obj_set_style_clip_corner(post_cont, true, 0);
            lv_obj_clear_flag(post_cont, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *scroll_area = lv_obj_create(post_cont);
            lv_obj_set_width(scroll_area, lv_pct(100));
            lv_obj_set_height(scroll_area, LV_SIZE_CONTENT);
            lv_obj_set_style_max_height(scroll_area, 125, 0);
            lv_obj_set_flex_flow(scroll_area, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(scroll_area, 0, 0);
            lv_obj_set_style_pad_gap(scroll_area, 0, 0);
            lv_obj_set_style_border_width(scroll_area, 0, 0);
            lv_obj_set_style_bg_opa(scroll_area, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(scroll_area, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_add_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE); 
            lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLL_ELASTIC); 
            lv_obj_set_scrollbar_mode(scroll_area, LV_SCROLLBAR_MODE_AUTO);

            char *fn_dup = (char *)lv_mem_alloc(entry.filename.length() + 1);
            strcpy(fn_dup, entry.filename.c_str());
            lv_obj_set_user_data(post_cont, fn_dup);

            lv_obj_set_style_border_width(post_cont, UI_BORDER_W, LV_STATE_FOCUSED);
            lv_obj_set_style_border_color(post_cont, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
            lv_obj_set_style_radius(post_cont, UI_RADIUS, 0);

            lv_group_add_obj(lv_group_get_default(), post_cont);
            lv_obj_add_event_cb(post_cont, post_focus_cb, LV_EVENT_FOCUSED, NULL);
            lv_obj_add_event_cb(post_cont, post_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
            lv_obj_add_event_cb(post_cont, post_click_cb, LV_EVENT_CLICKED, NULL);
            
            // Add internal scroll handler for editing mode
            lv_obj_add_event_cb(post_cont, [](lv_event_t *ev) {
                lv_obj_t *wrapper = (lv_obj_t *)lv_event_get_current_target(ev);
                lv_obj_t *scroller = lv_obj_get_child(wrapper, 0);
                lv_group_t *g = lv_group_get_default();
                if (!lv_group_get_editing(g) || lv_group_get_focused(g) != wrapper) return;

                uint32_t key = lv_event_get_key(ev);
                if (key == LV_KEY_UP || key == LV_KEY_LEFT) {
                    int32_t cur_y = lv_obj_get_scroll_y(scroller);
                    int32_t step = 40;
                    if (cur_y < step) step = cur_y;
                    if (step > 0) lv_obj_scroll_by(scroller, 0, step, LV_ANIM_ON);
                } else if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) {
                    int32_t bottom = lv_obj_get_scroll_bottom(scroller);
                    int32_t step = 40;
                    if (bottom < step) step = bottom;
                    if (step > 0) lv_obj_scroll_by(scroller, 0, -step, LV_ANIM_ON);
                }
            }, LV_EVENT_KEY, NULL);

            lv_obj_add_event_cb(post_cont, journal_key_event_cb, LV_EVENT_KEY, NULL);

            if (target_focus_index != -1) {
                if (current_child_idx == target_focus_index) {
                    focus_target = post_cont;
                } else if (current_child_idx < target_focus_index) {
                    focus_target = post_cont;
                }
            }

            lv_obj_t *header = lv_label_create(scroll_area);
            lv_label_set_text(header, parse_filename_to_human(entry.filename).c_str());
            lv_obj_set_style_text_font(header, font, 0);
            lv_obj_set_style_text_color(header, lv_palette_main(LV_PALETTE_GREY), 0);
            lv_obj_set_width(header, lv_pct(100));
            lv_obj_set_style_pad_left(header, 4, 0); 

            lv_obj_t *line = lv_obj_create(scroll_area);
            lv_obj_set_size(line, lv_pct(100), 1);
            lv_obj_set_style_bg_color(line, lv_palette_main(LV_PALETTE_GREY), 0);
            lv_obj_set_style_border_width(line, 0, 0);
            lv_obj_set_style_pad_all(line, 0, 0); 

            std::string preview = entry.snippet;
            lv_obj_t *label = lv_label_create(scroll_area);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_label_set_text(label, preview.c_str());
            lv_obj_set_width(label, lv_pct(100));
            lv_obj_set_style_text_font(label, font, 0);
            lv_obj_set_style_text_color(label, lv_color_white(), 0);
            lv_obj_set_style_pad_left(label, 4, 0); 
            lv_obj_set_style_pad_right(label, 4, 0);

            lv_obj_t *trunc_icon = lv_label_create(post_cont);
            lv_label_set_text(trunc_icon, LV_SYMBOL_DOWN);
            lv_obj_set_style_text_font(trunc_icon, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(trunc_icon, UI_COLOR_ACCENT, 0);
            lv_obj_add_flag(trunc_icon, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_align(trunc_icon, LV_ALIGN_BOTTOM_RIGHT, -12, -5);
            lv_obj_set_style_bg_opa(trunc_icon, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(trunc_icon, LV_OBJ_FLAG_HIDDEN);

            lv_obj_add_event_cb(scroll_area, post_scroll_indicator_cb, LV_EVENT_SCROLL, trunc_icon);
            trunc_checks.emplace_back(scroll_area, trunc_icon);
        }

        lv_obj_t *footer_cont = lv_obj_create(main_page);
        lv_obj_set_width(footer_cont, lv_pct(100));
        lv_obj_set_height(footer_cont, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(footer_cont, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(footer_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(footer_cont, 5, 0);
        lv_obj_set_style_bg_opa(footer_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(footer_cont, 0, 0);

        if (end_idx < total_files) {
            lv_obj_t *next_btn = lv_btn_create(footer_cont);
            lv_obj_t *next_label = lv_label_create(next_btn);
            lv_label_set_text(next_label, LV_SYMBOL_DOWN " Next");
            lv_obj_add_event_cb(next_btn, next_page_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), next_btn);
            lv_obj_add_event_cb(next_btn, post_focus_cb, LV_EVENT_FOCUSED, NULL);
        }

        lv_obj_t *page_info = lv_label_create(footer_cont);
        char buf[32];
        snprintf(buf, sizeof(buf), "Page %d/%d", current_page + 1, (total_files + JOURNAL_PAGE_SIZE - 1) / JOURNAL_PAGE_SIZE);
        lv_label_set_text(page_info, buf);
        lv_obj_set_style_text_font(page_info, font, 0);
        lv_obj_set_style_text_color(page_info, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_flex_grow(page_info, 1);
        lv_obj_set_style_text_align(page_info, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_pad_right(page_info, 10, 0);
    }

    lv_menu_set_page(menu, main_page);

    lv_obj_update_layout(main_page);
    for (auto &pair : trunc_checks) {
        lv_obj_t *scroll_area = pair.first;
        lv_obj_t *trunc_icon = pair.second;
        if (lv_obj_get_scroll_bottom(scroll_area) > 0) {
            lv_obj_remove_flag(trunc_icon, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
    }

    if (focus_target) {
        lv_group_focus_obj(focus_target);
        lv_obj_scroll_to_view(focus_target, LV_ANIM_OFF);
    }

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        do_exit();
    }, NULL);
#endif

    loader_hide();
}

void ui_journal_exit(lv_obj_t *parent)
{
    loader_hide();
    ui_hide_back_button();
    disable_keyboard();
    if (menu) {
        teardown_menu_for_rerender();
    } else if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    target_focus_index = -1;
    current_page = 0;
}

} // namespace

namespace apps {
class JournalApp : public core::App {
public:
    JournalApp() : core::App("Journal") {}
    void onStart(lv_obj_t *parent) override { ui_journal_enter(parent); }
    void onStop() override {
        ui_journal_exit(getRoot());
        core::App::onStop();
    }
};

APP_FACTORY(make_journal_app, JournalApp)
} // namespace apps
