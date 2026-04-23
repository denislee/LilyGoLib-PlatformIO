/**
 * @file      secrets.h
 * @brief     Encrypted NVS wrapper for device credentials.
 *
 * Stores short secrets (bearer tokens, PATs) as AES-256-CBC ciphertext under
 * caller-chosen (namespace, key) slots in NVS. Crypto is the same PBKDF2 +
 * passphrase session as notes_crypto — one unlock covers every slot. The
 * layout is intentionally identical to what ui_telegram.cpp / ui_notes_sync.cpp
 * wrote by hand, so existing NVS entries keep working.
 */
#pragma once

#include <string>

namespace hal {

/* True if ciphertext exists at (ns, key). Does not require the session to be
 * unlocked — safe to call at boot so UI can decide whether to prompt. */
bool secret_exists(const char *ns, const char *key);

/* Decrypt and return plaintext, scoped to the caller's frame. Returns empty
 * when the slot is missing, the session is locked, or the blob is corrupt.
 * The caller is responsible for scrubbing any copies it holds. */
std::string secret_load(const char *ns, const char *key);

/* Encrypt `plaintext` and persist. Passing null or "" clears the slot and
 * succeeds without needing the session unlocked. A non-empty write requires
 * notes_crypto enabled AND unlocked; on refusal returns false with *err set. */
bool secret_store(const char *ns, const char *key, const char *plaintext,
                  std::string *err = nullptr);

/* Remove just this slot. Does not touch siblings in the same namespace. */
void secret_erase(const char *ns, const char *key);

/* Drop a plaintext predecessor key that pre-dates encryption (e.g. the
 * bare "token" slot that lived next to "token_enc" during migration).
 * Idempotent — safe to call on every boot. */
void secret_purge_legacy(const char *ns, const char *legacy_key);

/* Overwrite a std::string's backing bytes before release so decrypted
 * plaintext doesn't linger in freed heap pages. */
void secret_scrub(std::string &s);

} // namespace hal
