/**
 * @file      notes_crypto.cpp
 * @brief     OpenSSL-compatible AES-256-CBC / PBKDF2-HMAC-SHA256 for notes.
 */
#include "notes_crypto.h"
#include "storage.h"
#include "../hal_interface.h"

#include <cstring>
#include <cstdio>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <LilyGoLib.h>
#include <FFat.h>
#include <SD.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>
#endif

/* The canary sits in its own NVS namespace so schema changes to the main
 * settings blob don't drag it along. */
#define NC_NS           "notes_sec"
#define NC_KEY_CANARY   "canary"
/* Matches `openssl enc -pbkdf2` default. Keep in lockstep with the host sync
 * script; if this changes, the sync script's openssl invocation must gain an
 * explicit `-iter` flag. */
#define NC_ITERATIONS   10000
#define NC_CANARY_PT    "NOTES_OK"
#define NC_SALT_LEN     8
#define NC_KEY_LEN      32
#define NC_IV_LEN       16
#define NC_MAGIC        "Salted__"

namespace {

bool g_unlocked = false;
std::string g_passphrase;   /* Held in RAM while unlocked. */

static void zeroize(void *buf, size_t n)
{
#ifdef ARDUINO
    mbedtls_platform_zeroize(buf, n);
#else
    volatile uint8_t *p = (volatile uint8_t *)buf;
    while (n--) *p++ = 0;
#endif
}

/* Strip one leading '/' so policy checks can ignore the anchor. */
static const char *lstrip_slash(const char *p)
{
    if (p && *p == '/') return p + 1;
    return p ? p : "";
}

static bool has_magic(const uint8_t *buf, size_t n)
{
    return n >= 8 && memcmp(buf, NC_MAGIC, 8) == 0;
}

/* ---- NVS canary helpers ---- */

static bool read_canary(std::vector<uint8_t> &out)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NC_NS, true)) return false;
    size_t len = p.getBytesLength(NC_KEY_CANARY);
    if (len == 0) { p.end(); return false; }
    out.resize(len);
    p.getBytes(NC_KEY_CANARY, out.data(), len);
    p.end();
    return true;
#else
    (void)out; return false;
#endif
}

static bool write_canary(const std::vector<uint8_t> &ct)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NC_NS, false)) return false;
    p.putBytes(NC_KEY_CANARY, ct.data(), ct.size());
    p.end();
    return true;
#else
    (void)ct; return false;
#endif
}

static void erase_canary()
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NC_NS, false)) return;
    p.remove(NC_KEY_CANARY);
    p.end();
#endif
}

/* ---- Crypto primitives ---- */

#ifdef ARDUINO
static bool derive_key_iv(const char *pw, const uint8_t salt[NC_SALT_LEN],
                          uint8_t key_out[NC_KEY_LEN], uint8_t iv_out[NC_IV_LEN])
{
    uint8_t kb[NC_KEY_LEN + NC_IV_LEN];

    /* Classic PKCS5 API (mbedTLS 2.x, ships with ESP-IDF 4.4). The newer
     * `_ext` variant exists in mbedTLS 3.x but this codebase targets the
     * older core. */
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;

    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    if (mbedtls_md_setup(&md, info, 1 /* use HMAC */) != 0) {
        mbedtls_md_free(&md);
        return false;
    }

    int r = mbedtls_pkcs5_pbkdf2_hmac(
        &md,
        (const unsigned char *)pw, strlen(pw),
        salt, NC_SALT_LEN,
        NC_ITERATIONS,
        sizeof(kb), kb);
    mbedtls_md_free(&md);

    if (r != 0) {
        zeroize(kb, sizeof(kb));
        return false;
    }
    memcpy(key_out, kb, NC_KEY_LEN);
    memcpy(iv_out,  kb + NC_KEY_LEN, NC_IV_LEN);
    zeroize(kb, sizeof(kb));
    return true;
}
#endif

static bool encrypt_with_pw(const char *pw,
                            const uint8_t *pt, size_t pt_len,
                            std::vector<uint8_t> &out)
{
#ifdef ARDUINO
    uint8_t salt[NC_SALT_LEN];
    esp_fill_random(salt, NC_SALT_LEN);

    uint8_t key[NC_KEY_LEN];
    uint8_t iv[NC_IV_LEN];
    if (!derive_key_iv(pw, salt, key, iv)) return false;

    /* PKCS7 pad to 16-byte block. */
    size_t pad = NC_IV_LEN - (pt_len % NC_IV_LEN);
    size_t body_len = pt_len + pad;
    std::vector<uint8_t> body(body_len);
    if (pt_len) memcpy(body.data(), pt, pt_len);
    for (size_t i = 0; i < pad; i++) body[pt_len + i] = (uint8_t)pad;

    std::vector<uint8_t> ct(body_len);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    bool ok = false;
    if (mbedtls_aes_setkey_enc(&ctx, key, 256) == 0) {
        uint8_t iv_work[NC_IV_LEN];
        memcpy(iv_work, iv, NC_IV_LEN);
        ok = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, body_len,
                                   iv_work, body.data(), ct.data()) == 0;
        zeroize(iv_work, sizeof(iv_work));
    }
    mbedtls_aes_free(&ctx);
    zeroize(key, sizeof(key));
    zeroize(iv, sizeof(iv));
    zeroize(body.data(), body.size());
    if (!ok) return false;

    out.clear();
    out.reserve(8 + NC_SALT_LEN + body_len);
    out.insert(out.end(), (const uint8_t *)NC_MAGIC, (const uint8_t *)NC_MAGIC + 8);
    out.insert(out.end(), salt, salt + NC_SALT_LEN);
    out.insert(out.end(), ct.begin(), ct.end());
    return true;
#else
    (void)pw;
    out.assign(pt, pt + pt_len);
    return true;
#endif
}

static bool decrypt_with_pw(const char *pw,
                            const uint8_t *ct, size_t ct_len,
                            std::string &out)
{
#ifdef ARDUINO
    /* Minimum: 8 magic + 8 salt + 1 block. */
    if (ct_len < 8 + NC_SALT_LEN + NC_IV_LEN) return false;
    if (!has_magic(ct, ct_len)) return false;
    size_t body_len = ct_len - 8 - NC_SALT_LEN;
    if (body_len == 0 || (body_len % NC_IV_LEN) != 0) return false;

    const uint8_t *salt = ct + 8;
    const uint8_t *body = ct + 8 + NC_SALT_LEN;

    uint8_t key[NC_KEY_LEN];
    uint8_t iv[NC_IV_LEN];
    if (!derive_key_iv(pw, salt, key, iv)) return false;

    std::vector<uint8_t> plain(body_len);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    bool ok = false;
    if (mbedtls_aes_setkey_dec(&ctx, key, 256) == 0) {
        uint8_t iv_work[NC_IV_LEN];
        memcpy(iv_work, iv, NC_IV_LEN);
        ok = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, body_len,
                                   iv_work, body, plain.data()) == 0;
        zeroize(iv_work, sizeof(iv_work));
    }
    mbedtls_aes_free(&ctx);
    zeroize(key, sizeof(key));
    zeroize(iv, sizeof(iv));
    if (!ok) {
        zeroize(plain.data(), plain.size());
        return false;
    }

    /* Strip PKCS7 pad with constant-ish checks. */
    uint8_t pad = plain.back();
    if (pad == 0 || pad > NC_IV_LEN || pad > body_len) {
        zeroize(plain.data(), plain.size());
        return false;
    }
    for (size_t i = 0; i < pad; i++) {
        if (plain[body_len - 1 - i] != pad) {
            zeroize(plain.data(), plain.size());
            return false;
        }
    }
    out.assign((const char *)plain.data(), body_len - pad);
    zeroize(plain.data(), plain.size());
    return true;
#else
    (void)pw;
    out.assign((const char *)ct, ct_len);
    return true;
#endif
}

/* ---- Raw FS helpers used by the migration routines.
 * These deliberately bypass the auto-crypto wrappers in hal/storage.cpp so we
 * can see the raw on-disk ciphertext during passphrase rotation. */

struct ProtFile {
    std::string path;
    bool on_sd;         /* false = FFat internal, true = SD card */
};

static std::string display_label(const ProtFile &pf)
{
    return (pf.on_sd ? std::string("SD: ") : std::string("")) + pf.path;
}

static bool raw_read(const ProtFile &pf, std::vector<uint8_t> &out)
{
    out.clear();
#ifdef ARDUINO
    String s = (!pf.path.empty() && pf.path[0] == '/')
               ? String(pf.path.c_str())
               : ("/" + String(pf.path.c_str()));
    File f;
    bool lock = false;
    if (pf.on_sd) {
        instance.lockSPI();
        f = SD.open(s, FILE_READ);
        lock = true;
    } else {
        f = FFat.open(s, FILE_READ);
    }
    if (!f) { if (lock) instance.unlockSPI(); return false; }
    size_t n = f.size();
    out.resize(n);
    if (n) f.read(out.data(), n);
    f.close();
    if (lock) instance.unlockSPI();
    return true;
#else
    (void)pf; return false;
#endif
}

static bool raw_write(const ProtFile &pf, const uint8_t *buf, size_t len)
{
#ifdef ARDUINO
    String s = (!pf.path.empty() && pf.path[0] == '/')
               ? String(pf.path.c_str())
               : ("/" + String(pf.path.c_str()));
    File f;
    bool lock = false;
    if (pf.on_sd) {
        instance.lockSPI();
        f = SD.open(s, "w");
        lock = true;
    } else {
        f = FFat.open(s, "w");
    }
    if (!f) { if (lock) instance.unlockSPI(); return false; }
    size_t w = f.write(buf, len);
    f.close();
    if (lock) instance.unlockSPI();
    return w == len;
#else
    (void)pf; (void)buf; (void)len; return true;
#endif
}

/* Walk the SD root for protected *.txt files. Mirrors hw_get_sd_txt_files but
 * kept inline so we can reuse the instance lock across the whole scan. */
#ifdef ARDUINO
static void collect_sd_protected(std::vector<ProtFile> &out)
{
    if (!(HW_SD_ONLINE & hw_get_device_online())) return;
    instance.lockSPI();
    File root = SD.open("/");
    if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String n = f.name();
                if (n.endsWith(".txt") &&
                    notes_crypto_path_is_protected(n.c_str())) {
                    out.push_back({std::string(n.c_str()), true});
                }
            }
            f.close();
            f = root.openNextFile();
        }
        root.close();
    }
    bool has_idx = SD.exists("/journal_idx.bin");
    instance.unlockSPI();
    if (has_idx) out.push_back({"/journal_idx.bin", true});
}
#endif

static void enumerate_protected(std::vector<ProtFile> &out)
{
    out.clear();
    std::vector<std::string> txt;
    hw_get_internal_txt_files(txt);
    for (auto &p : txt) {
        if (notes_crypto_path_is_protected(p.c_str())) out.push_back({p, false});
    }
#ifdef ARDUINO
    if (FFat.exists("/journal_idx.bin")) {
        out.push_back({"/journal_idx.bin", false});
    }
    /* Try to pick up the card if it wasn't mounted yet — a passphrase set
     * while the card was out should still migrate it on the next rotation. */
    if (!(HW_SD_ONLINE & hw_get_device_online())) {
        hw_mount_sd();
    }
    collect_sd_protected(out);
#endif
}

} /* namespace */

/* ===== Public API ===== */

bool notes_crypto_is_enabled()
{
    std::vector<uint8_t> c;
    return read_canary(c);
}

bool notes_crypto_is_unlocked()
{
    return g_unlocked;
}

bool notes_crypto_should_encrypt()
{
    /* Writes only ever produce ciphertext when the session is unlocked. If
     * encryption is enabled but we're locked, the caller must fail hard rather
     * than silently leak plaintext — see hal/storage.cpp. */
    return g_unlocked;
}

bool notes_crypto_path_is_protected(const char *path)
{
    if (!path) return false;
    const char *name = lstrip_slash(path);

    if (strcmp(name, "journal_idx.bin") == 0) return true;
    /* /news/* stays plaintext by policy (populated by the host sync script
     * which has no access to the passphrase on-device). */
    if (strncmp(name, "news/", 5) == 0) return false;
    /* Only top-level files — subdirectory contents are outside the journal
     * scope. */
    if (strchr(name, '/') != nullptr) return false;
    if (strcmp(name, "tasks.txt") == 0) return false;

    size_t n = strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".txt") == 0) return true;
    return false;
}

bool notes_crypto_unlock(const char *pw)
{
    if (!pw) return false;
    std::vector<uint8_t> canary;
    if (!read_canary(canary)) return false;

    std::string dec;
    if (!decrypt_with_pw(pw, canary.data(), canary.size(), dec)) return false;
    if (dec != NC_CANARY_PT) return false;

    g_passphrase = pw;
    g_unlocked = true;
    return true;
}

void notes_crypto_lock()
{
    if (!g_passphrase.empty()) {
        zeroize(&g_passphrase[0], g_passphrase.size());
    }
    g_passphrase.clear();
    g_unlocked = false;
}

bool notes_crypto_encrypt_buffer(const uint8_t *pt, size_t pt_len,
                                 std::vector<uint8_t> &out)
{
    if (!g_unlocked) return false;
    return encrypt_with_pw(g_passphrase.c_str(), pt, pt_len, out);
}

bool notes_crypto_decrypt_buffer(const uint8_t *ct, size_t ct_len,
                                 std::string &out)
{
    if (!g_unlocked) return false;
    return decrypt_with_pw(g_passphrase.c_str(), ct, ct_len, out);
}

bool notes_crypto_set_passphrase(const char *new_pw)
{
    if (!new_pw || !*new_pw) return false;
    /* Refuse if already enabled — caller should use change_passphrase. */
    std::vector<uint8_t> existing;
    if (read_canary(existing)) return false;

    std::vector<uint8_t> canary_ct;
    if (!encrypt_with_pw(new_pw,
                         (const uint8_t *)NC_CANARY_PT,
                         strlen(NC_CANARY_PT), canary_ct)) return false;
    if (!write_canary(canary_ct)) return false;

    g_passphrase = new_pw;
    g_unlocked = true;
    return true;
}

void notes_crypto_encrypt_existing(void (*cb)(int cur, int total, const char *name))
{
    if (!g_unlocked) return;

    std::vector<ProtFile> files;
    enumerate_protected(files);

    int total = (int)files.size();
    int cur = 0;
    for (const auto &pf : files) {
        cur++;
        std::string label = display_label(pf);
        if (cb) cb(cur, total, label.c_str());
        std::vector<uint8_t> raw;
        if (!raw_read(pf, raw)) continue;
        if (has_magic(raw.data(), raw.size())) continue;    /* already encrypted */

        std::vector<uint8_t> ct;
        if (!encrypt_with_pw(g_passphrase.c_str(),
                             raw.data(), raw.size(), ct)) continue;
        raw_write(pf, ct.data(), ct.size());
    }
    if (cb) cb(total, total, "Done");
}

bool notes_crypto_encrypt_sd(int *scanned, int *encrypted,
                             void (*cb)(int cur, int total, const char *name))
{
    if (scanned)   *scanned = 0;
    if (encrypted) *encrypted = 0;
    if (!notes_crypto_is_enabled() || !g_unlocked) return false;

#ifdef ARDUINO
    if (!(HW_SD_ONLINE & hw_get_device_online())) {
        hw_mount_sd();
    }
    if (!(HW_SD_ONLINE & hw_get_device_online())) return false;

    std::vector<ProtFile> files;
    collect_sd_protected(files);

    int total = (int)files.size();
    int cur = 0;
    int enc_count = 0;
    for (const auto &pf : files) {
        cur++;
        std::string label = display_label(pf);
        if (cb) cb(cur, total, label.c_str());

        std::vector<uint8_t> raw;
        if (!raw_read(pf, raw)) continue;
        if (has_magic(raw.data(), raw.size())) continue;    /* already encrypted */

        std::vector<uint8_t> ct;
        if (!encrypt_with_pw(g_passphrase.c_str(),
                             raw.data(), raw.size(), ct)) continue;
        if (raw_write(pf, ct.data(), ct.size())) enc_count++;
    }
    if (cb) cb(total, total, "Done");
    if (scanned)   *scanned   = total;
    if (encrypted) *encrypted = enc_count;
    return true;
#else
    (void)cb;
    return false;
#endif
}

bool notes_crypto_change_passphrase(const char *old_pw, const char *new_pw,
                                    void (*cb)(int cur, int total, const char *name))
{
    if (!old_pw || !new_pw || !*new_pw) return false;
    if (!notes_crypto_unlock(old_pw)) return false;

    std::string old_pass = old_pw;
    std::string new_pass = new_pw;

    std::vector<ProtFile> files;
    enumerate_protected(files);

    int total = (int)files.size();
    int cur = 0;
    for (const auto &pf : files) {
        cur++;
        std::string label = display_label(pf);
        if (cb) cb(cur, total, label.c_str());

        std::vector<uint8_t> raw;
        if (!raw_read(pf, raw)) continue;

        std::string plain;
        if (has_magic(raw.data(), raw.size())) {
            if (!decrypt_with_pw(old_pass.c_str(),
                                 raw.data(), raw.size(), plain)) continue;
        } else {
            plain.assign((const char *)raw.data(), raw.size());
        }

        std::vector<uint8_t> ct;
        if (!encrypt_with_pw(new_pass.c_str(),
                             (const uint8_t *)plain.data(), plain.size(), ct)) continue;
        raw_write(pf, ct.data(), ct.size());
        zeroize(&plain[0], plain.size());
    }

    /* Rotate the canary last — a crash between file rewrites and the canary
     * update would leave us with mixed keys under the OLD canary, which the
     * user can still recover from. */
    std::vector<uint8_t> canary_ct;
    if (!encrypt_with_pw(new_pass.c_str(),
                         (const uint8_t *)NC_CANARY_PT,
                         strlen(NC_CANARY_PT), canary_ct)) return false;
    if (!write_canary(canary_ct)) return false;

    zeroize(&old_pass[0], old_pass.size());
    g_passphrase = new_pass;
    zeroize(&new_pass[0], new_pass.size());
    g_unlocked = true;
    if (cb) cb(total, total, "Done");
    return true;
}

bool notes_crypto_disable(const char *pw, void (*cb)(int cur, int total, const char *name))
{
    if (!pw) return false;
    if (!notes_crypto_unlock(pw)) return false;

    std::vector<ProtFile> files;
    enumerate_protected(files);
    int total = (int)files.size();
    int cur = 0;
    for (const auto &pf : files) {
        cur++;
        std::string label = display_label(pf);
        if (cb) cb(cur, total, label.c_str());

        std::vector<uint8_t> raw;
        if (!raw_read(pf, raw)) continue;
        if (!has_magic(raw.data(), raw.size())) continue;  /* already plain */

        std::string plain;
        if (!decrypt_with_pw(g_passphrase.c_str(),
                             raw.data(), raw.size(), plain)) continue;
        raw_write(pf, (const uint8_t *)plain.data(), plain.size());
        zeroize(&plain[0], plain.size());
    }

    erase_canary();
    notes_crypto_lock();
    if (cb) cb(total, total, "Done");
    return true;
}
