#pragma once

// Single-key NVS (Arduino Preferences) accessors. The begin/get/put/end dance
// — plus its `#ifdef ARDUINO` / emulator-stub twin — used to be copy-pasted as
// `load_pref`/`save_pref` triplets in ui_telegram.cpp and ui_notes_sync.cpp
// (OPTIMIZATION_REPORT §2.18). Consolidated here so each app keeps only a thin
// namespace-binding wrapper. Multi-key configs (ui_ssh, ui_weather) deliberately
// keep their own batched begin/end — folding them into single-key calls would
// multiply NVS open/close cycles.
//
// Lives in HAL because it wraps a hardware/platform facility; on the desktop
// emulator (no NVS) the setters no-op and the getters return the default.

#include <string>

namespace hal {

// Read a string entry from `ns`/`key`, returning `dflt` when absent (or on the
// emulator). Opens the namespace read-only.
std::string nvs_get_str(const char *ns, const char *key, const char *dflt = "");

// Write a string entry. An empty/null `value` removes the key (matching the app
// configs' "empty means unset" convention).
void nvs_set_str(const char *ns, const char *key, const char *value);

// Read/write a bool entry, same namespace semantics as the string variants.
bool nvs_get_bool(const char *ns, const char *key, bool dflt);
void nvs_set_bool(const char *ns, const char *key, bool value);

} // namespace hal
