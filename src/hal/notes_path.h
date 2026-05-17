/**
 * @file      notes_path.h
 * @brief     Pure-logic helpers for notes-crypto path/format policy.
 *
 * Extracted from notes_crypto.cpp + storage.cpp so that the rules deciding
 * which files get encrypted (and how to detect already-encrypted bytes) can
 * be exercised by host-side unit tests without pulling in Arduino, mbedtls,
 * or the filesystem layer. Both functions are pure: no I/O, no globals.
 */
#pragma once

#include <cstddef>

/* True when this path falls under the notes encryption policy:
 *   - Exactly "journal_idx.bin" at the root.
 *   - Top-level "notes/<leaf>.txt" — a single level under notes/, .txt suffix.
 * Anything else (subdirectories of notes/, non-.txt files, paths outside
 * notes/) is left in plaintext. Leading slash on `path` is tolerated. */
bool notes_crypto_path_is_protected(const char *path);

/* True when `buf` begins with the OpenSSL `enc` "Salted__" magic. Used to
 * decide whether on-disk bytes are already ciphertext during read/migrate. */
bool content_has_salted_magic(const char *buf, size_t len);
