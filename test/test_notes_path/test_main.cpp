/**
 * @file      test_main.cpp
 * @brief     Unity tests for hal/notes_path.{h,cpp} — the pure-logic policy
 *            functions that decide which files are encrypted and whether
 *            on-disk bytes are already ciphertext.
 *
 * These two functions guard a security-relevant boundary (anything matched
 * by notes_crypto_path_is_protected gets encrypted on write and refused on
 * read when locked). Regressing the rules silently could either leak plain
 * notes or refuse to load valid ones. Cover both with golden cases.
 */
#include <unity.h>
#include <cstring>

#include "hal/notes_path.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- notes_crypto_path_is_protected ---- */

static void test_path_null_is_not_protected(void)
{
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected(nullptr));
}

static void test_path_empty_is_not_protected(void)
{
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected(""));
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("/"));
}

static void test_journal_idx_at_root_is_protected(void)
{
    TEST_ASSERT_TRUE(notes_crypto_path_is_protected("journal_idx.bin"));
    TEST_ASSERT_TRUE(notes_crypto_path_is_protected("/journal_idx.bin"));
}

static void test_journal_idx_in_subdir_is_not_protected(void)
{
    /* Only the root copy is the journal index. A same-named file in a
     * subdirectory is unrelated and stays in plaintext. */
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("notes/journal_idx.bin"));
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("/foo/journal_idx.bin"));
}

static void test_notes_txt_files_are_protected(void)
{
    TEST_ASSERT_TRUE(notes_crypto_path_is_protected("notes/hello.txt"));
    TEST_ASSERT_TRUE(notes_crypto_path_is_protected("/notes/hello.txt"));
    TEST_ASSERT_TRUE(notes_crypto_path_is_protected("notes/20260101_120000.txt"));
}

static void test_notes_non_txt_are_not_protected(void)
{
    /* The encryption policy is .txt-only. Other extensions (used for media
     * attachments, drafts, etc.) stay in plaintext. */
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("notes/photo.jpg"));
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("notes/draft.md"));
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("notes/hello"));
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("notes/.txt"));   /* dotfile, leaf too short */
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("notes/x.tx"));   /* truncated ext */
}

static void test_nested_notes_are_not_protected(void)
{
    /* The policy is exactly one level under notes/. A second slash means a
     * subdirectory, which is treated as user-organized scratch space. */
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("notes/sub/file.txt"));
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("notes/a/b/c.txt"));
}

static void test_outside_notes_dir_not_protected(void)
{
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("tasks.txt"));   /* root .txt, but not notes/ */
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("drafts/wip.txt"));
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("notesx/foo.txt")); /* lookalike prefix */
    TEST_ASSERT_FALSE(notes_crypto_path_is_protected("/note/foo.txt"));
}

/* ---- content_has_salted_magic ---- */

static void test_salted_magic_detects_openssl_header(void)
{
    /* OpenSSL `enc` writes ASCII "Salted__" then 8 bytes salt then ciphertext. */
    const char header[] = "Salted__\x01\x02\x03\x04\x05\x06\x07\x08";
    TEST_ASSERT_TRUE(content_has_salted_magic(header, sizeof(header) - 1));
}

static void test_salted_magic_rejects_short_buffer(void)
{
    TEST_ASSERT_FALSE(content_has_salted_magic("Salted", 6));
    TEST_ASSERT_FALSE(content_has_salted_magic("Salted_", 7));
    TEST_ASSERT_FALSE(content_has_salted_magic("", 0));
    TEST_ASSERT_FALSE(content_has_salted_magic(nullptr, 100));
}

static void test_salted_magic_rejects_plain_text(void)
{
    const char *plain = "Hello, world. This is a normal note.";
    TEST_ASSERT_FALSE(content_has_salted_magic(plain, strlen(plain)));
}

static void test_salted_magic_case_sensitive(void)
{
    /* `Salted__` is a literal byte sequence — case matters. A note that
     * accidentally starts with "salted__" lowercase should NOT be treated
     * as ciphertext. */
    TEST_ASSERT_FALSE(content_has_salted_magic("salted__deadbeef", 16));
    TEST_ASSERT_FALSE(content_has_salted_magic("SALTED__deadbeef", 16));
}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_path_null_is_not_protected);
    RUN_TEST(test_path_empty_is_not_protected);
    RUN_TEST(test_journal_idx_at_root_is_protected);
    RUN_TEST(test_journal_idx_in_subdir_is_not_protected);
    RUN_TEST(test_notes_txt_files_are_protected);
    RUN_TEST(test_notes_non_txt_are_not_protected);
    RUN_TEST(test_nested_notes_are_not_protected);
    RUN_TEST(test_outside_notes_dir_not_protected);
    RUN_TEST(test_salted_magic_detects_openssl_header);
    RUN_TEST(test_salted_magic_rejects_short_buffer);
    RUN_TEST(test_salted_magic_rejects_plain_text);
    RUN_TEST(test_salted_magic_case_sensitive);
    return UNITY_END();
}
