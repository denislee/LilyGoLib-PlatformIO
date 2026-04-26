/**
 * @file      ui_notes_sync.cpp
 * @brief     GitHub notes sync — additive push of on-device notes to a
 *            GitHub repo via the REST Contents API.
 *
 * The firmware can't run git, so we drive the push through the GitHub
 * REST Contents API over HTTPS:
 *   - GET /repos/{owner}/{repo}/contents/notes?ref=<branch> to learn
 *     which filenames are already present on the remote.
 *   - PUT /repos/{owner}/{repo}/contents/notes/{file} for every local
 *     note whose name is NOT already on the remote.
 *
 * Sources, in priority order: internal FFat first, SD card second.
 * When the same filename exists on both, FFat wins and the SD copy is
 * ignored — a just-edited internal note is what gets pushed even if a
 * stale SD copy with the same name still sits on the card.
 *
 * The push is strictly additive: files already on the remote are
 * skipped (never updated, never deleted). On-device edits to a name
 * already pushed will not propagate by themselves — by design, this
 * is a backup-only flow, not a mirror. To re-push a changed note,
 * delete or rename it on the remote first.
 *
 * Bytes go to the repo as raw ciphertext when notes crypto is enabled;
 * hw_read_internal_bytes_raw() / hw_read_sd_bytes_raw() bypass the
 * decrypt-on-read path so the repo gets the opaque Salted__ blob.
 *
 * The news-index download (denislee.github.io/hn) lives on the News app
 * itself — this sync only touches notes.
 */
#include "../ui_define.h"
#include "../hal/storage.h"
#include "../hal/system.h"
#include "../hal/wireless.h"
#include "../hal/notes_crypto.h"
#include "../hal/secrets.h"
#include "../core/app.h"
#include "../core/app_manager.h"
#include "../core/system.h"
#include "app_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Preferences.h>
#include <mbedtls/base64.h>
extern "C" {
#include "cJSON.h"
}
#endif

namespace {

#define NSYNC_PREFS_NS      "notesync"
#define NSYNC_DEFAULT_BRANCH "main"

// --- NVS helpers ----------------------------------------------------------

static std::string load_pref(const char *key, const char *dflt = "")
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NSYNC_PREFS_NS, true)) return dflt ? dflt : "";
    String v = p.getString(key, dflt ? dflt : "");
    p.end();
    return std::string(v.c_str());
#else
    (void)key;
    return dflt ? dflt : "";
#endif
}

static void save_pref(const char *key, const char *value)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NSYNC_PREFS_NS, false)) return;
    if (value && *value) p.putString(key, value);
    else p.remove(key);
    p.end();
#else
    (void)key; (void)value;
#endif
}

// GitHub PAT persistence delegates to hal/secrets — same encrypted-NVS
// wrapper the Telegram bearer uses, sharing the notes passphrase. A PAT
// with contents:write on a private repo is a write credential, so we
// never want it at rest plaintext; the legacy `token` slot is wiped at
// boot to close that door for any pre-hardening installs.
static bool token_is_encrypted() {
    return hal::secret_exists(NSYNC_PREFS_NS, "token_enc");
}

static std::string load_token_plain() {
    return hal::secret_load(NSYNC_PREFS_NS, "token_enc");
}

static bool save_token(const char *value, std::string *err) {
    // Keyboard / paste flows routinely append whitespace or a trailing newline;
    // baking that into the Authorization header yields a silent 401 from GitHub.
    std::string trimmed = value ? value : "";
    size_t a = trimmed.find_first_not_of(" \t\r\n");
    size_t b = trimmed.find_last_not_of(" \t\r\n");
    trimmed = (a == std::string::npos) ? std::string()
                                       : trimmed.substr(a, b - a + 1);
    bool ok = hal::secret_store(NSYNC_PREFS_NS, "token_enc",
                                trimmed.c_str(), err);
    if (ok) hal::secret_purge_legacy(NSYNC_PREFS_NS, "token");
    hal::secret_scrub(trimmed);
    return ok;
}

static void purge_legacy_plaintext_token() {
    hal::secret_purge_legacy(NSYNC_PREFS_NS, "token");
}

// Forward decl — the definition sits further down next to run_sync, but
// the HTTP helpers above need to scrub their Authorization header buffers
// the moment hw_http_request returns.
#ifdef ARDUINO
static void scrub_string(std::string &s);

// Pull the "message" field out of GitHub's error JSON ({"message":"Bad
// credentials","documentation_url":...}) so the log surfaces the real reason
// instead of a bare "HTTP 401". Falls back to a truncated body when the
// response isn't JSON, and finally to the hw_http_request error string.
static std::string gh_error(int code, const std::string &body,
                            const std::string &fallback)
{
    std::string msg;
    if (!body.empty()) {
        cJSON *j = cJSON_Parse(body.c_str());
        if (j) {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "message");
            if (m && cJSON_IsString(m) && m->valuestring) {
                msg = m->valuestring;
            }
            cJSON_Delete(j);
        }
        if (msg.empty()) {
            msg = body.substr(0, 120);
        }
    }
    if (msg.empty()) msg = fallback;
    char buf[24];
    snprintf(buf, sizeof(buf), "HTTP %d: ", code);
    return std::string(buf) + msg;
}
#endif

// --- base64 (mbedtls) -----------------------------------------------------

#ifdef ARDUINO
static bool b64_encode(const uint8_t *data, size_t len, std::string &out)
{
    size_t olen = 0;
    // Probe first to size the buffer — mbedtls writes the required length
    // into olen when dst is NULL/undersized.
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    out.resize(olen);
    size_t written = 0;
    int rc = mbedtls_base64_encode((unsigned char *)&out[0], out.size(),
                                   &written, data, len);
    if (rc != 0) { out.clear(); return false; }
    // The "written" count includes the null terminator space, but the
    // returned byte count does not — resize to the actual encoded bytes.
    out.resize(written);
    return true;
}
#endif

// Minimal JSON string escaper — only the characters JSON forbids verbatim
// appear in our payloads (paths, commit messages, base64). Control bytes
// never occur inside a commit message here, but escape them to be safe.
static std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// --- state ----------------------------------------------------------------

static lv_obj_t *s_root = nullptr;
static lv_obj_t *s_log_label = nullptr;
static lv_obj_t *s_log_scroll = nullptr;
static std::string s_log_text;
static bool s_syncing = false;

static void log_append(const char *line)
{
    if (line) s_log_text.append(line);
    s_log_text.push_back('\n');
    if (s_log_label) {
        lv_label_set_text(s_log_label, s_log_text.c_str());
        if (s_log_scroll) {
            lv_obj_update_layout(s_log_scroll);
            lv_obj_scroll_to_y(s_log_scroll, LV_COORD_MAX, LV_ANIM_OFF);
        }
    }
    lv_refr_now(nullptr);
}

static void log_appendf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_append(buf);
}

// --- config ---------------------------------------------------------------

struct Config {
    std::string repo;        // "owner/repo"
    std::string branch;      // default "main"
    std::string token;       // GitHub PAT (contents:write)
};

static bool load_config(Config &c, std::string *err)
{
    c.repo     = load_pref("repo");
    c.branch   = load_pref("branch", NSYNC_DEFAULT_BRANCH);
    c.token    = load_token_plain();
    if (c.branch.empty()) c.branch = NSYNC_DEFAULT_BRANCH;
    if (c.repo.empty()) {
        if (err) *err = "Repo not set (Settings » Notes Sync).";
        return false;
    }
    if (c.repo.find('/') == std::string::npos) {
        if (err) *err = "Repo must be owner/name.";
        return false;
    }
    if (c.token.empty()) {
        if (err) *err = token_is_encrypted()
                        ? "Token locked — open Notes to unlock."
                        : "GitHub token not set.";
        return false;
    }
    return true;
}

// --- GitHub API ------------------------------------------------------------

#ifdef ARDUINO

// Listing a folder that doesn't exist yet returns 404 with an empty body;
// callers shouldn't treat that as fatal — it just means the remote has no
// notes yet, so every local note is a candidate to push.

static bool list_remote_notes(const Config &c,
                              std::vector<std::string> &out,
                              std::string *err)
{
    out.clear();
    std::string url = std::string("https://api.github.com/repos/") + c.repo +
                      "/contents/notes?ref=" + c.branch;
    std::string auth = "Bearer " + c.token;
    std::string body;
    int code = 0;
    std::string terr;
    bool ok = hw_http_request(url.c_str(), "GET", nullptr, 0, nullptr,
                              auth.c_str(), body, &code, &terr);
    scrub_string(auth);
    if (!ok) {
        if (code == 404) return true;  // folder doesn't exist yet
        if (err) *err = gh_error(code, body, terr.empty() ? "list failed" : terr);
        return false;
    }
    cJSON *arr = cJSON_Parse(body.c_str());
    if (!arr) {
        if (err) *err = "Parse error";
        return false;
    }
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        out.reserve((size_t)n);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(arr, i);
            cJSON *jn = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *jt = cJSON_GetObjectItemCaseSensitive(it, "type");
            if (!jn || !cJSON_IsString(jn)) continue;
            if (jt && cJSON_IsString(jt) && strcmp(jt->valuestring, "file") != 0) continue;
            out.emplace_back(jn->valuestring);
        }
    }
    cJSON_Delete(arr);
    return true;
}

// Create notes/{name} on the remote with the given raw bytes. Additive-only:
// callers must have already confirmed the name does not exist remotely, so
// we never carry a prev-sha and a 422 here means somebody else raced us.
static bool put_remote_file(const Config &c,
                            const std::string &name,
                            const std::vector<uint8_t> &bytes,
                            std::string *err)
{
    std::string b64;
    if (!b64_encode(bytes.data(), bytes.size(), b64)) {
        if (err) *err = "base64 failed";
        return false;
    }
    std::string path = "notes/" + name;
    std::string msg = "sync: add " + name;

    std::string body;
    body.reserve(64 + b64.size());
    body += "{\"message\":\"";  body += json_escape(msg); body += "\",";
    body += "\"content\":\"";   body += b64;              body += "\",";
    body += "\"branch\":\"";    body += json_escape(c.branch); body += "\"}";

    std::string url = std::string("https://api.github.com/repos/") + c.repo +
                      "/contents/" + path;
    std::string auth = "Bearer " + c.token;
    std::string resp;
    int code = 0;
    std::string terr;
    bool ok = hw_http_request(url.c_str(), "PUT",
                              body.c_str(), body.size(),
                              "application/json",
                              auth.c_str(), resp, &code, &terr);
    scrub_string(auth);
    if (!ok) {
        if (err) *err = gh_error(code, resp, terr.empty() ? "PUT failed" : terr);
        return false;
    }
    return true;
}

#endif  // ARDUINO

// --- sync driver ----------------------------------------------------------

#ifdef ARDUINO
static void run_sync()
{
    std::string err;
    Config cfg;
    if (!load_config(cfg, &err)) {
        log_appendf("ERR: %s", err.c_str());
        return;
    }
    if (!hw_get_wifi_connected()) {
        log_append("ERR: WiFi not connected.");
        return;
    }

    log_appendf("Repo: %s  Branch: %s", cfg.repo.c_str(), cfg.branch.c_str());

    // 1. Build the local candidate list: FFat first, SD second. Same name on
    //    both sides → FFat wins (the SD copy is dropped from the candidate
    //    list and never read). The `internal` flag steers the raw read in
    //    step 4 to the right backing store.
    struct LocalNote {
        std::string name;
        bool internal;
    };

    auto strip_slash = [](std::string &p) {
        if (!p.empty() && p[0] == '/') p.erase(0, 1);
    };

    std::vector<std::string> ffat_files;
    hw_get_internal_txt_files(ffat_files);
    std::vector<LocalNote> local;
    local.reserve(ffat_files.size());
    for (auto &p : ffat_files) {
        strip_slash(p);
        if (!p.empty()) local.push_back({p, true});
    }
    unsigned ffat_count = (unsigned)local.size();

    unsigned sd_skipped_dup = 0;
    bool sd_online = (HW_SD_ONLINE & hw_get_device_online());
    if (sd_online) {
        std::vector<std::string> sd_files;
        hw_get_sd_txt_files(sd_files);
        for (auto &p : sd_files) {
            strip_slash(p);
            if (p.empty()) continue;
            bool dup = false;
            for (const auto &n : local) if (n.name == p) { dup = true; break; }
            if (dup) { sd_skipped_dup++; continue; }
            local.push_back({p, false});
        }
        log_appendf("Local notes: %u (FFat %u, SD %u, dup %u)",
                    (unsigned)local.size(), ffat_count,
                    (unsigned)local.size() - ffat_count, sd_skipped_dup);
    } else {
        log_appendf("Local notes: %u (FFat %u, SD offline)",
                    (unsigned)local.size(), ffat_count);
    }

    // 2. Fetch current remote state — names only; we never overwrite, so
    //    there's no need for the blob SHAs.
    std::vector<std::string> remote;
    if (!list_remote_notes(cfg, remote, &err)) {
        log_appendf("ERR: list: %s", err.c_str());
        return;
    }
    log_appendf("Remote notes: %u", (unsigned)remote.size());

    // 3. Additive push: upload each local file whose name is NOT already on
    //    the remote. Files already on the remote are skipped untouched —
    //    edits won't propagate by themselves, by design.
    int uploaded = 0, already = 0, skipped = 0, up_failed = 0;
    for (const auto &n : local) {
        bool on_remote = false;
        for (const auto &r : remote) if (r == n.name) { on_remote = true; break; }
        if (on_remote) {
            log_appendf("  skip %s (on remote)", n.name.c_str());
            already++;
            continue;
        }
        std::vector<uint8_t> bytes;
        std::string abs = "/" + n.name;
        bool ok = n.internal ? hw_read_internal_bytes_raw(abs.c_str(), bytes)
                             : hw_read_sd_bytes_raw(abs.c_str(), bytes);
        if (!ok) {
            log_appendf("  read %s: skip", n.name.c_str());
            skipped++;
            continue;
        }
        std::string perr;
        if (put_remote_file(cfg, n.name, bytes, &perr)) {
            log_appendf("  up %s (%c, %u B)", n.name.c_str(),
                        n.internal ? 'I' : 'S', (unsigned)bytes.size());
            uploaded++;
        } else {
            log_appendf("  up %s FAIL: %s", n.name.c_str(), perr.c_str());
            up_failed++;
        }
    }

    log_appendf("Notes: +%d =%d (fail %d, skip %d)",
                uploaded, already, up_failed, skipped);

    scrub_string(cfg.token);
}

static void scrub_string(std::string &s) { hal::secret_scrub(s); }
#endif  // ARDUINO

// --- UI -------------------------------------------------------------------

static void back_btn_cb(lv_event_t *) { menu_show(); }

static void sync_btn_cb(lv_event_t *)
{
    if (s_syncing) return;
    s_syncing = true;
    hw_feedback();
    s_log_text.clear();
    if (s_log_label) lv_label_set_text(s_log_label, "");
    log_append("Starting sync...");
#ifdef ARDUINO
    // Refuse to proceed unless the notes session is unlocked. load_token_plain
    // returns empty in the locked state, so run_sync would fail later
    // anyway, but checking up front gives the user a cleaner message and
    // avoids a wasted remote list call.
    if (token_is_encrypted() && !notes_crypto_is_unlocked()) {
        log_append("ERR: Notes session locked. Open Notes and unlock.");
    } else {
        run_sync();
    }
#else
    log_append("Not supported on emulator.");
#endif
    log_append("Done.");
    s_syncing = false;
}

static void enter(lv_obj_t *parent)
{
    s_root = parent;
    s_log_text.clear();
    ui_show_back_button(back_btn_cb);

    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 4, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Status banner: one-line summary of what the sync will do.
    lv_obj_t *status = lv_label_create(parent);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status, lv_pct(100));
    lv_obj_set_style_text_color(status, UI_COLOR_MUTED, 0);
    std::string repo = load_pref("repo");
    if (repo.empty()) {
        lv_label_set_text(status,
            "Not configured.\n"
            "Set the repo and token in Settings " "\xC2\xBB" " Notes Sync.\n"
            "Private repos work — create a fine-grained PAT with "
            "Contents: read/write on this repo only.");
    } else {
        // Plaintext is no longer a valid storage mode, but the label
        // still says what to do if crypto is disabled so the user
        // understands why the token reads as "not set".
        const char *tok_status;
        std::string tok = load_token_plain();
        if (!tok.empty()) {
            tok_status = "token: encrypted, unlocked";
        } else if (token_is_encrypted()) {
            tok_status = "token: locked";
        } else {
            tok_status = "token: not set";
        }
        // Scrub the scratch copy — we only fetched it to check emptiness.
#ifdef ARDUINO
        scrub_string(tok);
#endif
        lv_label_set_text_fmt(status, "%s  (%s)",
                              repo.c_str(), tok_status);
    }

    // Scrolling log area — PUT/DELETE/download calls each emit a line.
    s_log_scroll = lv_obj_create(parent);
    lv_obj_set_width(s_log_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_log_scroll, 1);
    lv_obj_set_style_bg_color(s_log_scroll, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(s_log_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_log_scroll, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_log_scroll, 1, 0);
    lv_obj_set_style_radius(s_log_scroll, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(s_log_scroll, 4, 0);
    lv_obj_set_scroll_dir(s_log_scroll, LV_DIR_VER);

    s_log_label = lv_label_create(s_log_scroll);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_log_label, lv_pct(100));
    lv_obj_set_style_text_color(s_log_label, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_log_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_log_label, "");
}

static void exit_cb(lv_obj_t *) {
    ui_hide_back_button();
    s_root = s_log_label = s_log_scroll = nullptr;
    s_log_text.clear();
    s_syncing = false;
}

static void on_unlocked_cb(bool ok, void *ud)
{
    // The enter() path reads the banner state from NVS + session; after
    // unlock we just rebuild the page so the banner flips from "locked"
    // to "encrypted" without a kick back to the menu. An unsuccessful
    // unlock leaves the banner as-is and Sync now refuses with a clear
    // message — no value in kicking back out of the app.
    lv_obj_t *parent = (lv_obj_t *)ud;
    if (!parent) return;
    lv_obj_clean(parent);
    enter(parent);
    if (ok) sync_btn_cb(nullptr);
}

class NotesSyncApp : public core::App {
public:
    NotesSyncApp() : core::App("Notes Sync") {}
    void onStart(lv_obj_t *parent) override {
        setRoot(parent);
        enter(parent);
#ifdef ARDUINO
        // Offer to unlock the notes session when the token is at rest
        // encrypted but the session is still locked. ui_passphrase_unlock
        // is a no-op (fires the callback immediately) when crypto is
        // disabled or already unlocked, so the normal case costs nothing.
        if (token_is_encrypted() && !notes_crypto_is_unlocked()) {
            ui_passphrase_unlock(on_unlocked_cb, parent);
        } else {
            sync_btn_cb(nullptr);
        }
#else
        sync_btn_cb(nullptr);
#endif
    }
    void onStop() override {
        exit_cb(getRoot());
        core::App::onStop();
    }
};

} // namespace

namespace apps {

APP_FACTORY(make_notes_sync_app, NotesSyncApp)

// Called once at startup by register_all(). Drops the legacy plaintext
// NVS slot so an older install that persisted the PAT unencrypted isn't
// left exposed on the next boot. Cheap no-op when there's nothing there.
void nsync_purge_legacy_token_slot()
{
#ifdef ARDUINO
    purge_legacy_plaintext_token();
#endif
}

// --- config helpers called from ui_settings.cpp ---------------------------

std::string nsync_cfg_get_repo()            { return load_pref("repo"); }
void        nsync_cfg_set_repo(const char *v)    { save_pref("repo", v ? v : ""); }

std::string nsync_cfg_get_branch()
{
    std::string v = load_pref("branch", NSYNC_DEFAULT_BRANCH);
    if (v.empty()) v = NSYNC_DEFAULT_BRANCH;
    return v;
}
void nsync_cfg_set_branch(const char *v) { save_pref("branch", v ? v : ""); }

std::string nsync_cfg_get_token_display()
{
    std::string tok = load_token_plain();
    if (tok.empty()) {
        return token_is_encrypted() ? "(locked)" : "(not set)";
    }
    if (tok.size() <= 8) return "********";
    return std::string("********") + tok.substr(tok.size() - 4);
}

bool nsync_cfg_set_token(const char *tok, std::string *err)
{
    return save_token(tok ? tok : "", err);
}

bool nsync_cfg_token_is_encrypted()         { return token_is_encrypted(); }

} // namespace apps
