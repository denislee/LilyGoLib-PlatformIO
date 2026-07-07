#pragma once

// Shared string-encoding helpers for building HTTP/JSON payloads. These used
// to be copy-pasted (byte-for-byte) across ui_chat.cpp, ui_notes_sync.cpp,
// ui_weather.cpp and hub.cpp — consolidated here per OPTIMIZATION_REPORT §2.18.
// They live in HAL because hub.cpp (HAL) is one consumer and apps may depend
// on HAL, but never the reverse.

#include <cstddef>
#include <cstdint>
#include <string>

namespace hal {

// Minimal JSON string escaper: escapes the characters JSON forbids verbatim
// (" \ and the C0 control bytes, with \n \r \t spelled out). Enough for the
// paths, commit messages and base64 blobs we put in request bodies.
std::string json_escape(const std::string &in);

// Percent-encode a value for use in a URL query. Unreserved characters
// (RFC 3986: A–Z a–z 0–9 - _ . ~) pass through; everything else, including
// UTF-8 bytes, is %XX-encoded.
std::string url_encode(const std::string &in);

#ifdef ARDUINO
// Base64-encode via mbedtls (ESP32 only — mbedtls is not linked into the
// desktop emulator, and every caller is already under #ifdef ARDUINO).
// Returns false and clears `out` on error.
bool base64_encode(const uint8_t *data, size_t len, std::string &out);
#endif

} // namespace hal
