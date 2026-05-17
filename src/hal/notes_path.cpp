/**
 * @file      notes_path.cpp
 * @brief     Implementation of notes_path.h — pure logic, no Arduino deps.
 */
#include "notes_path.h"

#include <cstring>

static const char *lstrip_slash(const char *p)
{
    if (p && *p == '/') return p + 1;
    return p ? p : "";
}

bool notes_crypto_path_is_protected(const char *path)
{
    if (!path) return false;
    const char *name = lstrip_slash(path);

    /* Journal index lives at the FFat root and stays there because it's
     * bookkeeping, not a user note. */
    if (strcmp(name, "journal_idx.bin") == 0) return true;

    /* User notes live one level deep under "notes/". Anything deeper is
     * outside this app's scope. */
    if (strncmp(name, "notes/", 6) == 0) {
        const char *leaf = name + 6;
        if (strchr(leaf, '/') != nullptr) return false;
        size_t n = strlen(leaf);
        return n > 4 && strcmp(leaf + n - 4, ".txt") == 0;
    }

    return false;
}

bool content_has_salted_magic(const char *buf, size_t len)
{
    return buf && len >= 8 && memcmp(buf, "Salted__", 8) == 0;
}
