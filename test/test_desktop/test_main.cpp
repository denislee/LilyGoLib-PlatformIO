/**
 * @file      test_main.cpp
 * @brief     Unity tests for hal/str_encode.{h,cpp} — the pure-logic string
 *            encoders shared by every HTTP/JSON body builder (chat, notes
 *            sync, weather, hub).
 *
 * Covers json_escape and url_encode only. hal::base64_encode is #ifdef
 * ARDUINO-gated (implemented via mbedtls, which isn't linked into
 * native_test) and every call site is itself under #ifdef ARDUINO, so it has
 * no portable pure-logic form to test here — left untested by construction,
 * not an oversight.
 */
#include <unity.h>

#include "hal/str_encode.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- json_escape ---- */

static void test_json_escape_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", hal::json_escape("").c_str());
}

static void test_json_escape_plain_passthrough(void)
{
    TEST_ASSERT_EQUAL_STRING("hello world", hal::json_escape("hello world").c_str());
}

static void test_json_escape_quote_and_backslash(void)
{
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c", hal::json_escape("a\"b\\c").c_str());
}

static void test_json_escape_named_control_chars(void)
{
    TEST_ASSERT_EQUAL_STRING("\\n\\r\\t", hal::json_escape("\n\r\t").c_str());
}

static void test_json_escape_other_control_char_as_u_escape(void)
{
    // 0x01 has no named escape, so it falls through to \u00XX.
    TEST_ASSERT_EQUAL_STRING("\\u0001", hal::json_escape("\x01").c_str());
}

static void test_json_escape_utf8_bytes_pass_through(void)
{
    // UTF-8 continuation bytes are >= 0x20 as unsigned char, so they're not
    // control-escaped — only the JSON-forbidden ASCII characters are.
    const char *euro = "\xE2\x82\xAC"; // U+20AC in UTF-8
    TEST_ASSERT_EQUAL_STRING(euro, hal::json_escape(euro).c_str());
}

/* ---- url_encode ---- */

static void test_url_encode_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", hal::url_encode("").c_str());
}

static void test_url_encode_unreserved_passthrough(void)
{
    const char *unreserved = "AZaz09-_.~";
    TEST_ASSERT_EQUAL_STRING(unreserved, hal::url_encode(unreserved).c_str());
}

static void test_url_encode_space_and_reserved(void)
{
    TEST_ASSERT_EQUAL_STRING("a%20b%2Fc%3Fd", hal::url_encode("a b/c?d").c_str());
}

static void test_url_encode_utf8_bytes_percent_encoded(void)
{
    // U+20AC (Euro sign) is E2 82 AC in UTF-8 — every byte gets %XX-encoded.
    TEST_ASSERT_EQUAL_STRING("%E2%82%AC", hal::url_encode("\xE2\x82\xAC").c_str());
}

static void test_url_encode_hex_digits_are_uppercase(void)
{
    // Byte 0xAB is not unreserved, so it must round-trip as "%AB", not "%ab".
    TEST_ASSERT_EQUAL_STRING("%AB", hal::url_encode("\xAB").c_str());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_json_escape_empty);
    RUN_TEST(test_json_escape_plain_passthrough);
    RUN_TEST(test_json_escape_quote_and_backslash);
    RUN_TEST(test_json_escape_named_control_chars);
    RUN_TEST(test_json_escape_other_control_char_as_u_escape);
    RUN_TEST(test_json_escape_utf8_bytes_pass_through);

    RUN_TEST(test_url_encode_empty);
    RUN_TEST(test_url_encode_unreserved_passthrough);
    RUN_TEST(test_url_encode_space_and_reserved);
    RUN_TEST(test_url_encode_utf8_bytes_percent_encoded);
    RUN_TEST(test_url_encode_hex_digits_are_uppercase);

    return UNITY_END();
}
