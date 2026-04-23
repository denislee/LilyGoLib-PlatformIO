/**
 * @file      ui_notes_sync.cpp
 * @brief     GitHub notes sync — the on-device counterpart of the host
 *            lilygo-notes-sync.sh script.
 *
 * The script mounts the device's USB MSC, copies `*.txt` from the mount
 * root into a git repo, downloads the news index onto the card, then
 * commits + pushes. The firmware can't run git, so we drive the same
 * workflow through the GitHub REST Contents API over HTTPS:
 *   - Upload every top-level note on the SD card via PUT
 *     /repos/{owner}/{repo}/contents/notes/{file} (carrying the previous
 *     SHA when the file already exists so GitHub treats it as an update,
 *     not a conflict).
 *   - Delete anything under notes/ on the repo side that no longer
 *     exists on the SD card.
 *   - Download the news index HTML and every *.txt link from it into
 *     /news on the device (SD preferred; FFat fallback via
 *     hw_http_download_to_file).
 *
 * The card is the sync source of truth — that matches what the host
 * script sees when it mounts the MSC, and it keeps notes that only live
 * in internal FFat from being pushed to a shared repo. Bytes go to the
 * repo as raw ciphertext when notes crypto is enabled — we read via
 * hw_read_sd_bytes_raw() which bypasses the decrypt-on-read path.
 */
#include "ui_define.h"
#include "hal/storage.h"
#include "hal/system.h"
#include "hal/wireless.h"
#include "hal/notes_crypto.h"
#include "hal/secrets.h"
#include "core/app.h"
#include "core/app_manager.h"
#include "core/system.h"
#include "apps/app_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Preferences.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
extern "C" {
#include "cJSON.h"
}
#endif

namespace {

#define NSYNC_PREFS_NS      "notesync"
#define NSYNC_DEFAULT_NEWS  "https://denislee.github.io/hn/"
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
// GitHub's Contents API reports a file's `sha` as the Git blob SHA-1, which
// hashes the bytes `"blob <len>\0<content>"` — not a plain SHA-1 over the
// file. Computing it locally lets us skip uploads when the bytes already
// match the remote, which otherwise produces a fresh commit per file on
// every sync regardless of changes.
static std::string git_blob_sha1(const uint8_t *data, size_t len)
{
    char header[32];
    int hlen = snprintf(header, sizeof(header), "blob %u", (unsigned)len);
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, (const unsigned char *)header, (size_t)hlen + 1);
    if (len) mbedtls_sha1_update(&ctx, data, len);
    unsigned char out[20];
    mbedtls_sha1_finish(&ctx, out);
    mbedtls_sha1_free(&ctx);
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.resize(40);
    for (int i = 0; i < 20; i++) {
        s[i * 2]     = hex[(out[i] >> 4) & 0xF];
        s[i * 2 + 1] = hex[out[i] & 0xF];
    }
    return s;
}

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
static lv_obj_t *s_sync_btn = nullptr;
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
    std::string news_url;    // HTML index of news .txt files
};

static bool load_config(Config &c, std::string *err)
{
    c.repo     = load_pref("repo");
    c.branch   = load_pref("branch", NSYNC_DEFAULT_BRANCH);
    c.news_url = load_pref("news_url", NSYNC_DEFAULT_NEWS);
    c.token    = load_token_plain();
    if (c.branch.empty()) c.branch = NSYNC_DEFAULT_BRANCH;
    if (c.news_url.empty()) c.news_url = NSYNC_DEFAULT_NEWS;
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

// Listing a folder that doesn't exist yet returns 404 with an empty body; a
// caller shouldn't treat that as fatal — it just means nothing to reconcile.
struct RemoteEntry {
    std::string name;
    std::string sha;
};

static std::string api_url(const Config &c, const std::string &path)
{
    // GitHub treats leading/trailing slashes literally — path is already of
    // the form "notes" or "notes/foo.txt".
    return std::string("https://api.github.com/repos/") + c.repo +
           "/contents/" + path + "?ref=" + c.branch;
}

static bool list_remote_notes(const Config &c,
                              std::vector<RemoteEntry> &out,
                              std::string *err)
{
    out.clear();
    std::string url = api_url(c, "notes");
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
            cJSON *js = cJSON_GetObjectItemCaseSensitive(it, "sha");
            cJSON *jt = cJSON_GetObjectItemCaseSensitive(it, "type");
            if (!jn || !cJSON_IsString(jn)) continue;
            if (jt && cJSON_IsString(jt) && strcmp(jt->valuestring, "file") != 0) continue;
            RemoteEntry e;
            e.name = jn->valuestring;
            if (js && cJSON_IsString(js)) e.sha = js->valuestring;
            out.push_back(std::move(e));
        }
    }
    cJSON_Delete(arr);
    return true;
}

// Upload (create or update) notes/{name} with the given raw bytes. When the
// file already exists on the remote, `prev_sha` must be the blob SHA so the
// Contents API treats this as an update — passing an empty sha for an
// existing file makes GitHub reject the call with a 422.
static bool put_remote_file(const Config &c,
                            const std::string &name,
                            const std::vector<uint8_t> &bytes,
                            const std::string &prev_sha,
                            std::string *err)
{
    std::string b64;
    if (!b64_encode(bytes.data(), bytes.size(), b64)) {
        if (err) *err = "base64 failed";
        return false;
    }
    std::string path = "notes/" + name;
    std::string msg = "sync: update " + name;

    std::string body;
    body.reserve(64 + b64.size() + prev_sha.size());
    body += "{\"message\":\"";  body += json_escape(msg); body += "\",";
    body += "\"content\":\"";   body += b64;              body += "\",";
    body += "\"branch\":\"";    body += json_escape(c.branch); body += "\"";
    if (!prev_sha.empty()) {
        body += ",\"sha\":\""; body += prev_sha; body += "\"";
    }
    body += "}";

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

static bool delete_remote_file(const Config &c,
                               const std::string &name,
                               const std::string &sha,
                               std::string *err)
{
    std::string path = "notes/" + name;
    std::string msg = "sync: remove " + name;
    std::string body;
    body += "{\"message\":\"";  body += json_escape(msg); body += "\",";
    body += "\"sha\":\"";       body += sha;              body += "\",";
    body += "\"branch\":\"";    body += json_escape(c.branch); body += "\"}";

    std::string url = std::string("https://api.github.com/repos/") + c.repo +
                      "/contents/" + path;
    std::string auth = "Bearer " + c.token;
    std::string resp;
    int code = 0;
    std::string terr;
    bool ok = hw_http_request(url.c_str(), "DELETE",
                              body.c_str(), body.size(),
                              "application/json",
                              auth.c_str(), resp, &code, &terr);
    scrub_string(auth);
    if (!ok) {
        if (err) *err = gh_error(code, resp, terr.empty() ? "DELETE failed" : terr);
        return false;
    }
    return true;
}

// Parse href="YYYY-MM-DD.txt" out of the news index HTML. Matches
// parse_remote_index in ui_news.cpp but only returns the filenames — we
// don't need the size string here.
static void parse_news_index(const std::string &html,
                             std::vector<std::string> &out)
{
    out.clear();
    const char *needle = ".txt\">TXT</a>";
    size_t pos = 0;
    while ((pos = html.find(needle, pos)) != std::string::npos) {
        size_t href_end = pos + 4;
        size_t href_start = html.rfind("href=\"", pos);
        if (href_start == std::string::npos) { pos++; continue; }
        href_start += 6;
        std::string fname = html.substr(href_start, href_end - href_start);
        if (fname.size() == 14 && fname[4] == '-' && fname[7] == '-') {
            out.push_back(std::move(fname));
        }
        pos = href_end;
    }
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
    if (!(HW_SD_ONLINE & hw_get_device_online())) {
        log_append("ERR: SD card not mounted. Insert the card and retry.");
        return;
    }

    log_appendf("Repo: %s  Branch: %s", cfg.repo.c_str(), cfg.branch.c_str());

    // 1. Enumerate top-level *.txt notes on the SD card. The card is the
    //    sync source of truth — matches the host script, which only sees
    //    what USB MSC exposes.
    std::vector<std::string> local;
    hw_get_sd_txt_files(local);

    // Strip the leading slash so filenames match GitHub's response shape.
    for (auto &p : local) {
        if (!p.empty() && p[0] == '/') p.erase(0, 1);
    }
    log_appendf("Local notes: %u", (unsigned)local.size());

    // 2. Fetch current remote state.
    std::vector<RemoteEntry> remote;
    if (!list_remote_notes(cfg, remote, &err)) {
        log_appendf("ERR: list: %s", err.c_str());
        return;
    }
    log_appendf("Remote notes: %u", (unsigned)remote.size());

    // 3. Upload each local file whose content differs from the remote.
    //    Skip unchanged files — a blind PUT creates a no-op commit on
    //    GitHub even when bytes match, so syncing a dozen notes with one
    //    edit used to produce a dozen commits. Compare the Git blob SHA
    //    locally to the `sha` we got from the listing.
    int uploaded = 0, unchanged = 0, skipped = 0, up_failed = 0;
    for (const auto &fname : local) {
        std::vector<uint8_t> bytes;
        std::string abs = "/" + fname;
        if (!hw_read_sd_bytes_raw(abs.c_str(), bytes)) {
            log_appendf("  read %s: skip", fname.c_str());
            skipped++;
            continue;
        }
        std::string prev_sha;
        for (const auto &r : remote) {
            if (r.name == fname) { prev_sha = r.sha; break; }
        }
        if (!prev_sha.empty() &&
            git_blob_sha1(bytes.data(), bytes.size()) == prev_sha) {
            unchanged++;
            continue;
        }
        std::string perr;
        if (put_remote_file(cfg, fname, bytes, prev_sha, &perr)) {
            log_appendf("  up %s (%u B)", fname.c_str(), (unsigned)bytes.size());
            uploaded++;
        } else {
            log_appendf("  up %s FAIL: %s", fname.c_str(), perr.c_str());
            up_failed++;
        }
    }

    // 4. Delete remote files that no longer exist locally.
    int deleted = 0, del_failed = 0;
    for (const auto &r : remote) {
        bool still_local = false;
        for (const auto &fname : local) {
            if (fname == r.name) { still_local = true; break; }
        }
        if (still_local) continue;
        std::string derr;
        if (delete_remote_file(cfg, r.name, r.sha, &derr)) {
            log_appendf("  rm %s", r.name.c_str());
            deleted++;
        } else {
            log_appendf("  rm %s FAIL: %s", r.name.c_str(), derr.c_str());
            del_failed++;
        }
    }

    log_appendf("Notes: +%d -%d =%d (fail +%d -%d, skip %d)",
                uploaded, deleted, unchanged, up_failed, del_failed, skipped);

    // Token is no longer needed once the contents API calls are done —
    // the news download is a public HTTPS GET. Scrub now so the PAT
    // doesn't sit in heap for the duration of the news download loop.
    scrub_string(cfg.token);

    // 5. News: fetch the index HTML, download every .txt into /news. Any
    //    prior files stay put — the host script rm -rf's /news first, but
    //    doing that on-device risks wiping files we can't re-download when
    //    the index call partly fails.
    log_appendf("News: fetching %s", cfg.news_url.c_str());
    std::string html, herr;
    if (!hw_http_get_string(cfg.news_url.c_str(), html, &herr)) {
        log_appendf("News: index fetch failed: %s", herr.c_str());
        return;
    }
    std::vector<std::string> news;
    parse_news_index(html, news);
    if (news.empty()) {
        log_append("News: no files in index.");
        return;
    }
    int n_ok = 0, n_fail = 0;
    for (const auto &fname : news) {
        std::string url = cfg.news_url;
        if (!url.empty() && url.back() != '/') url.push_back('/');
        url += fname;
        std::string dest = "/news/" + fname;
        std::string derr;
        if (hw_http_download_to_file(url.c_str(), dest.c_str(),
                                     nullptr, nullptr, &derr)) {
            n_ok++;
        } else {
            log_appendf("  news %s FAIL: %s", fname.c_str(), derr.c_str());
            n_fail++;
        }
    }
    log_appendf("News: %d/%d files (%d failed).",
                n_ok, (int)news.size(), n_fail);
}

static void scrub_string(std::string &s) { hal::secret_scrub(s); }
#endif  // ARDUINO

// --- UI -------------------------------------------------------------------

static void back_btn_cb(lv_event_t *) { menu_show(); }

static void sync_btn_cb(lv_event_t *)
{
    if (s_syncing) return;
    s_syncing = true;
    if (s_sync_btn) lv_obj_add_state(s_sync_btn, LV_STATE_DISABLED);
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
    if (s_sync_btn) lv_obj_clear_state(s_sync_btn, LV_STATE_DISABLED);
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

    s_sync_btn = create_button(parent, LV_SYMBOL_REFRESH,
                                "Sync now", sync_btn_cb);
    lv_group_t *grp = lv_group_get_default();
    if (grp) lv_group_add_obj(grp, s_sync_btn);

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
    lv_obj_set_style_text_font(s_log_label, get_small_font(), 0);
    lv_label_set_text(s_log_label, "");

    if (grp) lv_group_focus_obj(s_sync_btn);
}

static void exit_cb(lv_obj_t *) {
    ui_hide_back_button();
    s_root = s_log_label = s_log_scroll = s_sync_btn = nullptr;
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

std::shared_ptr<core::App> make_notes_sync_app() {
    return std::make_shared<NotesSyncApp>();
}

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

std::string nsync_cfg_get_news_url()
{
    std::string v = load_pref("news_url", NSYNC_DEFAULT_NEWS);
    if (v.empty()) v = NSYNC_DEFAULT_NEWS;
    return v;
}
void nsync_cfg_set_news_url(const char *v) { save_pref("news_url", v ? v : ""); }

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
