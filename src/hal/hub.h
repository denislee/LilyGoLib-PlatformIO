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

#include "result.h"

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
// not cache. BLOCKS for up to `timeout_ms`; do not call on the LVGL thread —
// prefer hub_last_reachable() there.
bool hub_is_reachable(uint32_t timeout_ms = 1500);

// Non-blocking read of the most recent reachability verdict. The status-bar
// timer probes the hub every ~10 s on a background task and publishes the
// result via hub_note_reachable(); UI-thread hot paths (telegram poll/send,
// etc.) read this cached value instead of doing their own blocking connect().
// Returns false when the hub is disabled, no probe has completed yet, or the
// last verdict is older than `max_age_ms` (a stale verdict is treated as down,
// so callers safely fall back to the direct path).
bool hub_last_reachable(uint32_t max_age_ms = 30000);

// Publish a fresh reachability verdict into the cache. Called by whoever runs
// the periodic background probe (today, the status-bar timer). Thread-safe.
void hub_note_reachable(bool reachable);

// POST raw note bytes to the hub at /api/notes/upload. The hub stores them
// under its notes dir keyed by `name` (overwrites on second upload). Used to
// mirror flash-resident notes when internal storage is being pruned, and as
// the source of truth for the new notes-sync flow that no longer touches the
// SD card.
//
// Returns HalError::Ok on a 2xx response. On failure the caller can use
// `hal_error_string(err)` for a user-facing message or branch on specific
// causes (HubDisabled / WifiOffline / HttpError / Unauthorized / ...).
HalError hub_upload_note(const char *name, const uint8_t *bytes, size_t len);

} // namespace hal
