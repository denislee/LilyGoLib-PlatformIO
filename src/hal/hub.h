/**
 * @file      hub.h
 * @brief     Local hub (lilyhub) configuration — single source of truth.
 *
 * The hub is a cross-cutting service: weather first, notes-sync next, more
 * later. Storing its config in feature-specific NVS namespaces (originally
 * "weather/hub_url") leaks that abstraction. This module owns the "hub" NVS
 * namespace and is the only place feature code should read or write hub state.
 *
 * Migration: on first read of the URL, if "hub/url" is unset and the legacy
 * "weather/hub_url" slot has a value, the URL is copied over, the toggle is
 * forced ON (preserving old behavior — if it was set, the user wanted it on),
 * and the legacy slot is deleted. Idempotent.
 */
#pragma once

#include <cstdint>
#include <string>

namespace hal {

// True iff the master toggle is ON *and* a non-empty URL is configured. This
// is what feature code should branch on — "should I try the hub?".
bool hub_is_enabled();

// Configured base URL, trimmed of trailing slashes/whitespace. Returns "" when
// hub is disabled or no URL is set. Feature code calls this and short-circuits
// on empty without consulting the toggle separately.
std::string hub_get_url();

// Raw accessors for the settings UI: these reflect what's stored regardless
// of whether the hub would currently be considered "enabled".
bool hub_get_enabled_pref();
std::string hub_get_url_raw();

// Setters. hub_set_url("") clears the slot. hub_set_enabled(false) leaves the
// URL untouched so a user can toggle off and on without re-typing.
void hub_set_enabled(bool enabled);
void hub_set_url(const char *url);

// TCP-probe the configured hub. Cheap — a single connect() with a short
// timeout, no HTTP request. Returns false when the hub is disabled, WiFi
// isn't up, the URL can't be parsed, or the connection fails. Callers (e.g.
// the status-bar timer) are responsible for throttling — this function does
// not cache.
bool hub_is_reachable(uint32_t timeout_ms = 1500);

// POST raw note bytes to the hub at /api/notes/upload. The hub stores them
// under its notes dir keyed by `name` (overwrites on second upload). Used to
// mirror flash-resident notes when internal storage is being pruned, and as
// the source of truth for the new notes-sync flow that no longer touches the
// SD card. Returns true on a 2xx response. `error` (optional) receives a
// short diagnostic on failure.
bool hub_upload_note(const char *name, const uint8_t *bytes, size_t len,
                     std::string *error = nullptr);

} // namespace hal
