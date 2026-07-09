# Optimization Phase 2 — Analysis & Handoff

Generated **2026-07-09** at commit `9feb1c8` by a five-way analysis pass (UI-thread
blocking, memory/heap, HAL/task/power, Go server, flash/build), with every headline
claim spot-verified against current source. This is the handoff for the **next**
optimization arc, now that phase 1 is code-complete.

**Relationship to the other docs:**
- `OPTIMIZATION_REPORT.md` — phase-1 analysis (2026-07-05), **stale**, kept for history.
- `OPTIMIZATION_PROGRESS.md` — phase-1 tracker. §1 dead code, §2 perf, §3 build/assets
  are done; its Methodology section (grep discipline, the three near-regressions) still
  applies verbatim to this phase — **read it before removing or changing anything**.
- This file — fresh findings, numbered `P2.x` to avoid collision with phase-1 `§n.m`.

**Baseline:** Flash 70.4 % (2,954,809 B) · RAM 27.4 % static (89,696 B). App partition
is dual-slot OTA, 4 MiB each (`lib/LilyGoLib/examples/factory/partitions.csv`), so
there is **1.18 MiB flash headroom — no size emergency**; flash items below are
comfort/product wins. The scarce resource is **internal heap** (WiFi/TLS contend for
it) and **idle power**.

**Build discipline (unchanged):** after every change,
`pio run -e tlora_pager && pio run -e emulator_lora_pager && pio test -e native_test`.
Anything touching task loops / ISRs / boot is ⚠️ **hardware-test-required** — add it to
the smoke-test checklist in `OPTIMIZATION_PROGRESS.md` (which itself is still pending
a device run for the phase-1 radio/audio/§2.13/§2.17 passes).

---

## Executive summary

| # | Finding | Impact | Risk |
|---|---|---|---|
| P2.1 | Keyboard task polls at 100 Hz **through fake sleep** (no gate) | Sleep power + lock contention | Low fix / ⚠️ HW test |
| P2.2 | NFC task takes instance lock 50×/s even with NFC disabled | Lock contention, wasted wakeups | Low |
| P2.3 | Settings "Test Internet" = 3 s synchronous TCP on LVGL thread | 3 s hard UI freeze | Low (pattern exists) |
| P2.4 | cJSON has no allocator hooks → every JSON parse lands in internal heap | ~15–30 KB internal spikes during TLS | Low |
| P2.5 | GPS rail + UART held on 24/7 when `gps_enable` set, no consumer | Tens of mA continuous | Product decision |
| P2.6 | `playerTask` 8 KB internal stack reserved at boot, forever | 3–8 KB internal RAM | Low |
| P2.7 | File browser: full dir re-scan per filter toggle; uncapped widgets | Freezes + heap on big dirs | Low–Med |
| P2.8 | Bulk storage/crypto ops on UI thread with **no watchdog yield** | TWDT panic on large corpus | Low (1-line mitigation) |
| P2.9 | Journal reconcile runs on the LVGL thread ("bg" = deferred, not off-thread) | Jank after dirty FS | Med |
| P2.10 | Flash: drop `montserrat_40` (1 call site) + PNG/JPEG/BMP decoders | −102 KB measured (est. −129 KB), no feature loss ✅ | Low |
| P2.11 | Flash: font-picker cap / face trims (product decisions, now measured) | up to −150 KB more | Med |
| P2.12 | WiFi power-save never configured; fake sleep keeps default PS | Sleep power | Product decision |
| §4 | Go server: D1–D4 ✅ done (SSRF, unbounded maps, path traversal, timeouts); D5 + A1/A2 cosmetics remain | Security + Pi OOM | Low fixes |

---

## Status

| # | Status |
|---|---|
| P2.3 | ✅ done — `internet_test_click_cb` (`settings_connectivity.cpp`) now spawns a one-shot `internet_test_task` worker + 100 ms `internet_test_drain_tick` timer, mirroring the weather city-search pattern (`settings_weather.cpp`) instead of the home-screen pill pattern (`menu_app.cpp`) since this is a subpage, not the always-alive menu app. In-flight-check re-click reattaches the drain and shows "Still testing…"; task-spawn failure falls back to the old synchronous probe; emulator has no FreeRTOS so stays inline. `internet_test_teardown()` wired into `reset_state()` so re-entering the subpage never leaks the timer. Verified: `pio run -e tlora_pager`, `pio run -e emulator_lora_pager`, `pio test -e native_test` all pass. No hardware test required (UI-thread-only change, same probe call, no ISR/task-loop/boot path touched). |
| P2.4 | ✅ done — `hw_init()` (`src/hal/system.cpp`, top of the `#ifdef ARDUINO` block, ahead of RTC sync and every app) now installs a `cJSON_Hooks` routing cJSON allocation to PSRAM: `heap_caps_malloc(sz, MALLOC_CAP_SPIRAM)`, falling back to `MALLOC_CAP_8BIT` (internal DRAM) only if PSRAM is momentarily exhausted so a parse never fails outright. Free hook is stdlib `free`, which is heap-agnostic in the Arduino-ESP32 unified allocator — verified against the existing `free(req_str)` on the PSRAM-printed body in `ui_telegram.cpp:509` (the sole `cJSON_Print*` caller). Moves the ~15–30 KB internal-heap JSON spike (weather/telegram/chat/notes-sync parse sites) into PSRAM, off the internal heap WiFi/TLS contend for; all parse sites `cJSON_Delete` before returning, so nothing long-lived changes residence. Added `#include <esp_heap_caps.h>` + `#include "cJSON.h"` to the ARDUINO include block. Emulator/native: gated out by `#ifdef ARDUINO`. Verified: `pio run -e tlora_pager` (RAM 27.4 %/89,712 B, Flash 70.5 %/2,955,737 B), `pio run -e emulator_lora_pager`, `pio test -e native_test` (19/19) all pass. No ISR/task-loop/boot-ordering change; worth an on-device smoke check that weather/telegram/chat still parse (they exercise the new allocator). |
| P2.2 | ✅ done (code) — `nfc_task_fn` (`src/hal/nfc_task.cpp`) now gates on `if (ui_is_fake_sleep() || !hw_nfc_discovery_active())` **before** taking the instance lock and blocks on `ulTaskNotifyTake(200 ms)` instead of spinning at 50 Hz. Reused the existing `hw_nfc_discovery_active()` accessor (reads `g_discovery_active`) — no new state. `hw_start_nfc_discovery()` (`hal/peripherals.cpp`) kicks the poller via the new `hw_nfc_task_notify_wake()` so enabling NFC starts scanning immediately instead of waiting out the 200 ms idle timeout; no-op stubs added for the `!USING_ST25R3916` and `!ARDUINO` branches. Removes ~50 pointless instance-lock acquisitions/s (the default-off common case) plus the 50×/s fake-sleep wakeups. Verified: `pio run -e tlora_pager` (RAM 27.4 %/89,712 B · Flash 70.5 %/2,955,937 B), `pio run -e emulator_lora_pager`, `pio test -e native_test` (19/19) all pass. ⚠️ **HW-test-required** (touches a task loop) — added to the smoke-test checklist in `OPTIMIZATION_PROGRESS.md`. |
| P2.1 | ✅ done (code) — `keyboard_task_fn` (`src/hal/keyboard_task.cpp`) now checks `ui_is_fake_sleep()` at the top of the loop and blocks on `ulTaskNotifyTake(200 ms)` instead of polling the TCA8418 at 100 Hz through fake-sleep. `ui_resume_timers()` (`ui_main.cpp`) kicks it via the new `hw_keyboard_task_notify_wake()` (alongside the existing `hw_lvgl_task_notify_wake()`) so the first keypress after wake isn't delayed. Two correctness guards beyond the doc snippet: `xLastWakeTime` is reset after the block so `vTaskDelayUntil` doesn't burst to "catch up" skipped ticks on wake, and `s_held_char` is cleared so a key held before sleep can't fire a stale auto-repeat on wake. Declared `hw_keyboard_task_notify_wake()` in `core/system_hooks.h`; added `#include "../core/system_hooks.h"` to the task TU; no-op stubs for the `!USING_INPUT_DEV_KEYBOARD` and `!ARDUINO` branches. Removes ~100 core-0 wakeups/s + I2C reads to a powered-off chip during fake-sleep. Verified: same three-target build/test pass as P2.2. ⚠️ **HW-test-required** (task loop + first-keypress latency) — on the smoke-test checklist. |
| P2.8 | ✅ done (minimal mitigation) — `storage_progress_cb` (`src/apps/settings_storage.cpp`) is the single callback every bulk/crypto flow feeds one call per file (verified: `hw_copy_all_notes_to_hub`/`hw_copy_internal_to_sd` in `storage_bulk.cpp`, `hw_prune_internal_storage` in `storage.cpp:689`, and all four `notes_crypto_*` loops in `notes_crypto.cpp:715-867` call `cb(cur,total,name)` at the top of each iteration). It previously did an unconditional `lv_refr_now(NULL)` and **never yielded**, so a large corpus starved IDLE0/IDLE1 and could panic the TWDT (not just stutter). Now it throttles both the screen flush **and** a `vTaskDelay(1)` IDLE yield to a >100 ms window using `lv_tick_get()`/`lv_tick_elaps()` — the exact guard `ui_journal.cpp`'s `report()` uses. The progress widgets still update their value every call (bar stays accurate); only the flush+yield are throttled. `vTaskDelay(1)` is `#ifdef ARDUINO`-gated (emulator: LVGL-only, no FreeRTOS). Fixing the shared callback covers all seven flagged sites in one edit; no HAL/crypto loop bodies were touched. Verified: `pio run -e tlora_pager` (RAM 27.4 %/89,712 B · Flash 70.5 %/2,955,973 B), `pio run -e emulator_lora_pager`, `pio test -e native_test` (19/19) all pass. Not a task-loop/ISR/boot change (established LVGL-thread pattern), so low-risk; still worth an on-device smoke check driving a large-corpus encrypt-all / copy-to-SD to confirm no TWDT panic. **Full fix (worker + drain for copy-to-hub network I/O) remains deferred** per the section. |
| P2.10 | ✅ done (items 1+2; flash, −102 KB) — **Item 1:** `lv_font_montserrat_40` had exactly one call site (verified) — the `LV_SYMBOL_WARNING` glyph on the USB "Unsafe to disconnect" screen (`core/system.cpp:367`). Retargeted to `montserrat_32` (already pinned by clock/audio code → zero flash cost, and it stays larger than the `_28` title below it), swapped the file's `LV_FONT_DECLARE(_40)` → `_32`, and set `LV_FONT_MONTSERRAT_40 0` in `lv_conf.h`. Flash 2,955,973 → 2,885,793 B (**−70,180 B**, matches the doc's −70,177 B estimate). **Item 2:** disabled the three unconditionally-registered, non-gc-able image decoders `LV_USE_LODEPNG`/`LV_USE_TJPGD`/`LV_USE_BMP`; also flipped `LV_USE_GIF`/`LV_USE_QRCODE` → 0 (already gc'd, 0 bytes — honesty). Verified the firmware never decodes a PNG/JPEG/BMP file: no `lv_qrcode`/`lv_gif`/`lv_bmp`/`lv_lodepng`/`lv_tjpgd` API refs and no `A:`/`S:` file-path image sources in `src/` (the only `.jpg` strings are emulator mock dir-listings in `storage.cpp`; the only `lv_img_set_src` takes `LV_SYMBOL_*` glyphs). Flash 2,885,793 → 2,853,693 B (**−32,100 B** — below the doc's −58,787 B `nm`-estimate, which over-counted shared/collectible code). Combined **−102,280 B** (70.5 % → 68.0 %); RAM unchanged (89,712 B). Two separate commits per the order. Verified: `pio run -e tlora_pager`, `pio run -e emulator_lora_pager`, `pio test -e native_test` (19/19) all pass. Pure build-config/flash change — no HW test required (the USB-eject screen still renders, just with a 32 px warning glyph). |
| §4 (D1–D4) | ✅ done — the Go-server security + OOM pass, verified with `gofmt -l` (clean), `go vet` (clean), and `go test -race` (all packages green). **D1** (`telegram.go`): replaced the `HasPrefix(url,"http")` SSRF hole with an `allowedTelegramURL` allowlist (scheme `https` + host `api.telegram.org`, case-insensitive; userinfo/suffix tricks rejected via `Hostname()`), and now relays Telegram's status+JSON body verbatim instead of flattening non-200s to a text/plain 502; dropped the now-unused `fmt`/`truncate`. **D3** (`notessync.go`): strict `validRepo` (owner/name, no `.`/`..` segment) replacing `Contains("/")`, `safeName` applied per-file in `runSync` (invalid → per-file error, additive/retry-safe), and `url.PathEscape`(name)/`url.QueryEscape`(branch) when building the GitHub URLs. **D2** (`cache.go`/`weather.go`/`chat.go`): 512-entry cap + eviction on the weather cache (expired-first, else nearest-expiry; RWMutex read path untouched) plus canonicalized keys (forecast lat/lon rounded 2dp + params sorted, keeping every response-affecting param so no collisions; geosearch lowercased), and a 256-session LRU cap on chat (`evictSessionsLocked`, oldest `updated`; new session stamped before the check so it's never its own victim). **D4** (`main.go`): added `ReadTimeout` 60s + `IdleTimeout` 120s (WriteTimeout left off for the ≤60s chat upstream). Added regression tests for all four (SSRF allowlist, repo/name traversal, cache bound, session eviction) — the `cache`/`telegram`/`notessync` packages had none. **Singleflight (thundering-herd, part of D2a) intentionally deferred** — the entry cap already closes the OOM vector and a hand-rolled singleflight is more surface than this pass warranted; noted in §D. Four commits (one per item). |
| P2.5–P2.7, P2.9, P2.11–P2.12, §4-D5/A1/A2 | Not started — see sections below and the suggested execution order. |

---

## A. Firmware — concurrency & power (highest value)

### P2.1 — Keyboard task polls at 100 Hz through fake sleep ⚠️ HW-test-required — ✅ done (code); see Status table

`src/hal/keyboard_task.cpp:88-102` — the loop is `vTaskDelayUntil(10 ms)` and its only
gate is `hw_get_device_online() & HW_KEYBOARD_ONLINE` (:94). It **never checks
`ui_is_fake_sleep()`**, unlike rotary (`rotary_task.cpp:69-72`), charge
(`charge_task.cpp:131-142`) and lvgl (`lvgl_task.cpp:51-57`) which all suspend.

On fake-sleep entry `hw_power_down_all()` powers the keyboard rail off and
`instance.kb.end()` detaches the TCA8418 ISR — but `HW_KEYBOARD_ONLINE` is a one-shot
boot probe bit (`LilyGo_LoRa_Pager.cpp:271,987`) that `kb.end()` does not clear, so the
task keeps running: **~100 core-0 wakeups/s, each taking/releasing the instance mutex
(:102), plus an I2C read every 100 ms to a powered-off chip** — exactly while the
firmware is trying to save power.

**Fix:** mirror the rotary pattern at the top of the loop:
```cpp
if (ui_is_fake_sleep()) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200)); continue; }
```
plus a `hw_keyboard_task_notify_wake()` called from `ui_resume_timers()` (alongside the
existing `hw_lvgl_task_notify_wake()`), so the first keypress after wake isn't delayed.
Include `core/system_hooks.h`.

**Verify on hardware:** first-keypress latency after wake; no missed keys.
Related (lower priority): even awake, 9 of 10 lock acquisitions guard a pure
early-return (vendor driver only touches I2C every 100 ms). Raising `kPollMs` 10→20-30 ms
halves/thirds the lock churn at no perceptible latency cost — separate, tiny commit.

### P2.2 — NFC task: 50 locks/s for a no-op when NFC is disabled — ✅ done (code); see Status table

`src/hal/nfc_task.cpp:36-46` — loop: `vTaskDelay(20 ms)` → fake-sleep `continue` →
`core::ScopedInstanceLock` → `loopNFCReader()`, which immediately early-returns on
`!_nfc_running` (`nfc_reader.cpp:131-133`). NFC is off by default, so the common state
is **50 instance-lock acquisitions per second, forever, for nothing** — contending
with the keyboard task and LVGL render. During fake sleep it still wakes 50×/s (the
`continue` runs after the delay).

**Fix:** expose a cheap `bool hw_nfc_running()` (reads `_nfc_running` /
`g_discovery_active`, `hal/peripherals.cpp:119`) and check it **before** taking the
lock; when false (or fake-sleeping), idle at ~200 ms or block on a task notify kicked
by `hw_set_nfc_enable(true)`. Tag-tap responsiveness when enabled is unchanged.
Combined with P2.1 this removes ~150 pointless core-0 wakeups/s during "sleep".

### P2.3 — Settings › Connectivity "Test Internet": 3 s synchronous freeze

`src/apps/settings_connectivity.cpp:146-168` — `internet_test_click_cb` paints
"Testing…" via `lv_refr_now(NULL)` (:151) then calls
`hw_ping_internet("1.1.1.1", 53, 3000, …)` **inline on the LVGL thread** (:155).
WiFi-associated-but-offline ⇒ full 3 s freeze (encoder, keyboard, back button dead) —
precisely the situation in which a user taps this button.

This is the **twin of the already-fixed home-screen ping** (§2.10): `menu_app.cpp:235`
(`inet_check_task` one-shot worker) + `inet_check_drain` (100 ms `lv_timer`, :300)
even keeps a synchronous fallback for the task-spawn-failure case. Copy that pattern
verbatim; results land in the status label instead of a popup. Cleanest first commit
of the phase.

### P2.4 — Route cJSON to PSRAM (one init call, biggest internal-heap win)

**No `cJSON_InitHooks` call exists anywhere in `src/` or the vendor lib** (verified by
grep). cJSON allocates one small node + duplicated strings per JSON element; small
allocations are forced into **internal DRAM** by the Arduino-ESP32 unified allocator,
PSRAM notwithstanding. An Open-Meteo forecast (~5 KB JSON) expands to roughly
15–30 KB of internal-heap nodes; Telegram chats/messages similar — and these spikes
happen on the network workers **while WiFi/TLS buffers are live**, i.e. at peak
internal-heap pressure.

Parse sites: `ui_weather.cpp:488,615,836,1263`, `ui_telegram.cpp:744,764,1430,1654,1826`,
`ui_chat.cpp:460`, `ui_notes_sync.cpp:294`.

**Fix (in `hw_init()`, `src/hal/system.cpp:204`, before any app runs):**
```c
static cJSON_Hooks hooks = {
    [](size_t sz){ return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); },
    free  // capability-agnostic in the unified heap
};
cJSON_InitHooks(&hooks);
```
All parse sites `cJSON_Delete` before returning, so nothing long-lived changes
residence. Emulator branch: not needed (`#ifdef ARDUINO`). PSRAM is slower than DRAM,
but JSON parsing is not a hot loop here.

### P2.5 — GPS powered 24/7 when enabled, with no continuous consumer

`src/hal/system.cpp:249,651` assert `POWER_GPS` at boot and on every wake when
`user_setting.gps_enable` is set, and `Serial1` opens at 38400 (`hal/sensors.cpp:18-28`)
— but the **only** consumer is `hw_pump_time_sync_gps()`, called solely from the
date/time settings screen (`settings_datetime.cpp:218`). The transient sync path
already self-powers GPS on demand and releases it
(`hal/gps_time_sync.cpp:78-86` + `release_transient_gps_power()`). Net: the toggle
burns tens of mA continuously for nothing.

**Fix:** let the transient sync own GPS power; don't latch the rail for `gps_enable`.
**Product decision** — changes what the GPS settings toggle means. Bench-measure to
quantify the win before/after.

### P2.6 — `playerTask`: 8 KB internal stack reserved at boot, forever

`src/hal/audio.cpp:328` — `xTaskCreate(playerTask, "app/play", 8*1024, …)` runs from
`hw_audio_init()` at boot and never exits; it just blocks on `playerQueue`
(:299-319). The recorder task is already on-demand + self-deleting — the player isn't.
Two options: (a) measure `uxTaskGetStackHighWaterMark` during MP3 playback and trim to
~4–5 KB (the big decode buffers moved to PSRAM in §2.15, so the high-water is likely
low), or (b) spawn on first `APP_EVENT_PLAY` and self-delete after idle, mirroring the
recorder. (a) is the safe first step. ⚠️ verify playback on hardware after trimming.

---

## B. Firmware — UI-thread & memory (medium)

### P2.7 — File browser: redundant disk re-scans + uncapped widget build

`src/apps/ui_file_browser.cpp`:
1. `filter_click_cb` (:323) calls `load_entries()` (:328) — a **full SD/FFat directory
   re-scan** (holding `ScopedSpiLock` for the whole `openNextFile()` loop,
   `storage.cpp:547/614`) — just to re-apply a client-side filter (:115-119) to data it
   already had. Cache the raw listing for the current dir; make the filter buttons
   re-filter + `refresh_ui()` only.
2. `refresh_ui` (:262-296) creates ~3 LVGL objects per entry with **no cap**; LVGL's
   object heap is PSRAM-backed on this build (`LV_Helper_v9.cpp:434-436`) so this is
   mostly a latency problem, but hundreds of entries = hundreds of ms frozen while
   holding the SPI lock (which also stalls display flush). Cap rendered rows (e.g.
   first 200 by the existing mtime sort) with a "… N more" footer.
3. Bigger (optional): move `load_entries` to a worker + drain like `weather_bg_task`.
   Navigation is user-initiated, so items 1–2 may be enough.

Same class, lower priority: `ui_audio_notes.cpp:134` (`reload_notes`) scans on the UI
thread — usually one small dir, leave unless it shows up in practice.

### P2.8 — Bulk storage/crypto ops: no watchdog yield (crash vector) — ✅ minimal fix done; see Status table

`src/apps/settings_storage.cpp` click handlers run whole-corpus synchronous loops on
the LVGL thread behind a modal: copy-to-hub (:103 → `hw_copy_all_notes_to_hub`, N ×
file-read + HTTP), copy-to-SD (:127), prune (:86), and the four notes-crypto
passphrase/encrypt-all flows (:295,312,344,381). Blocking behind a modal is by-design
UX **but none of these loops feed the idle task** — `storage_progress_cb` only does
`lv_refr_now(NULL)`. Contrast `ui_journal.cpp:175-185` (`report()`), which calls
`vTaskDelay(1)` every 100 ms specifically to keep the task watchdog fed. A large
corpus can **panic the TWDT**, not just stutter.

**Minimal fix (do this first):** add the same throttled `vTaskDelay(1)` into
`storage_progress_cb` / the bulk loops in `hal/storage_bulk.cpp` + crypto loops.
**Full fix (later, optional):** worker + drain for copy-to-hub specifically (it does
network I/O on the UI thread).

### P2.9 — Journal reconcile: "background" ≠ off-thread

`src/apps/ui_journal.cpp:410-424` — `bg_refresh_timer_cb` is an `lv_timer`, i.e. it
runs `refresh_journal_entries` (dir scan + per-changed-note snippet decrypt,
:189-282) **on the LVGL thread**, deferred one frame. It yields via `report()` so the
watchdog is safe, but the list janks for seconds after a dirty FS (post-sync) with
hundreds of notes. Fix: produce the new `entries` vector on a FreeRTOS worker, swap +
re-render from a drain timer; the existing `entries_signature` (:386) already gates
the re-render. Only worth it if post-sync jank is noticeable in practice.

Adjacent, conditional: `journal_entries` (static, :289) holds up to 1536 B snippet per
note (`kSnippetBytes`, :44) and persists after app exit by design (instant reopen).
For heavy corpora that is potentially ~100s of KB of internal heap. Options: shrink
snippets toward what the list actually renders, move snippet storage to PSRAM, or
clear on exit. **§2.8 already shrank this once — confirm current behavior before
touching.**

### P2.10 — Notes-sync assembles the full multi-note upload body in RAM

`src/apps/ui_notes_sync.cpp:243-259` — `body.reserve(256 + 1024 * local.size())`,
then base64 of every local note appended into one `std::string` before POST. Scales
with note count, transient, on the sync worker (not the UI thread). Fix = chunked
uploads or streaming the POST body; needs server coordination. Medium effort — pair it
with the §4/B3 Git-Data-API work if that happens.

**Re-flagged but staying deferred:** `hw_http_request`/`hw_http_get_string`
double-buffer (`wireless.cpp:465-466, 523-524` — Arduino `String` + `std::string`
copy of every response, 2× body in internal heap during TLS teardown). This is
phase-1 **§2.16, deferred for poor risk/reward**; the deferral reasoning in
`OPTIMIZATION_PROGRESS.md` stands. Revisit only if heap watermarks show TLS-time
pressure *after* P2.4 lands (P2.4 removes the bigger spike at the same moment).

---

## C. Flash (no urgency — 1.18 MiB headroom; ranked by bytes/risk)

Measured from the linked ELF (`xtensa-esp32s3-elf-nm`/`size` on
`.pio/build/tlora_pager/firmware.elf`). Phase-1 flash work (Montserrat 34–46 off,
demos off, dead lib_deps) is done; these are the genuinely untapped items.

1. ~~**Drop `lv_font_montserrat_40` — −70,177 B, LOW risk.**~~ ✅ **done — −70,180 B
   measured.** Retargeted the sole call site (`core/system.cpp:367`, USB "unsafe to
   disconnect" warning glyph) to `_32` and set `LV_FONT_MONTSERRAT_40 0` in
   `lv_conf.h:500`.
2. ~~**Disable unused image decoders — −58,787 B, LOW risk.**~~ ✅ **done — −32,100 B
   measured** (the `nm`-estimate over-counted shared/collectible code). Set
   `LV_USE_LODEPNG`/`LV_USE_TJPGD`/`LV_USE_BMP` → 0 and flipped `LV_USE_GIF`/
   `LV_USE_QRCODE` → 0 (already gc'd). Confirmed the app decodes no PNG/JPEG/BMP
   files (only `LV_SYMBOL_*` glyphs and C-array images).
3. **Cap font picker at 24 — −74,873 B, MEDIUM (product decision).** Only `_26`
   (32,905 B) and `_30` (41,968 B) are picker-only; `_28`/`_32` are pinned by
   clock/audio code and **must stay** (the old "~118 KB" estimate is not reachable).
   Trim `FONT_SIZE_OPTIONS` (`settings_fonts.cpp:20`) + the 26/30 switch cases in
   `pick_font` (`ui_tools.cpp:850-853`) + lv_conf flags.
4. **Drop one redundant monospace face — −31,258 B (JBMono) or −27,904 B (Courier),
   LOW-MED (product).** Both are monospace; keep one. Remove from `FONT_FACE_OPTIONS`
   (`settings_fonts.cpp:19`), its `pick_font` branch, and `init_icon_fallback_fonts`
   (`ui_tools.cpp:758-778`).
5. **Emoji fonts — −43,072 B, MED (product):** breaks emoji in Telegram/chat.
6. **Do NOT touch:** `montserrat_48` (96.7 KB but 6 real big-display call sites),
   Atkinson/Inter (only Latin-1 faces; Inter is the emoji base).

**Confirmed already-optimal (don't re-investigate):** LVGL heap is PSRAM
(`LV_STDLIB_CUSTOM` → `ps_malloc`, `LV_Helper_v9.cpp:434-436`; the `LV_MEM_SIZE`
line is dead config); widget flags are gc'd (chart/calendar/etc. absent from ELF);
no TLS cert bundle in the image (`wireless.cpp:399,492` use `setInsecure()` — a
**security note, not a size lever**: all app HTTPS is unvalidated by design); `-Os`
is the platform default; LTO not recommended against prebuilt framework libs;
RadioLib already pared via `RADIOLIB_EXCLUDE_*`.

### Product / power decisions to surface to the user
- **P2.12 WiFi power-save:** zero `esp_wifi_set_ps`/`WiFi.setSleep` calls in `src/` —
  running at the Arduino default (`WIFI_PS_MIN_MODEM`). Option: switch to
  `WIFI_PS_MAX_MODEM` on fake-sleep entry, restore on resume. Trades notification
  latency for battery; gate behind a setting.
- **§3.1 picker cap / face drops** — items 3–5 above.
- **P2.5 GPS toggle semantics** — above.

---

## D. Go server (`server/`) — all phase-1 §4 items verified still open, plus 2 new

**D1–D4 are ✅ done** (see the §4 Status row; `gofmt -l` now clean, `go vet` clean,
`go test -race` green, and the `cache`/`telegram`/`notessync` packages gained their
first tests). Items 5–6 (Git Data API + cosmetics) remain. Fix order:

1. ✅ **done — B6 telegram proxy SSRF** (`internal/telegram/telegram.go`): was
   `strings.HasPrefix(req.URL, "http")` — any LAN client could relay to any host
   (metadata endpoints, localhost admin ports…) with a caller-supplied bearer. Now
   an `allowedTelegramURL` allowlist (scheme `https` + host `api.telegram.org`,
   case-insensitive; userinfo/suffix tricks rejected via `Hostname()`), and upstream
   status + JSON body relayed verbatim instead of flattened to a text/plain 502.
2. ✅ **done (except singleflight) — unbounded maps = Pi OOM vectors.**
   (a) Weather cache (`internal/cache/cache.go`): now a **512-entry cap** with eviction
   (expired-first, else nearest-expiry; no per-`Get` bookkeeping so the RWMutex read
   path is unchanged) + **canonicalized keys** (`weather.go`): forecast lat/lon rounded
   to 2dp and params sorted — **all** response-affecting params kept, so no cache
   collisions — geosearch lowercased. Upstream URL still uses the raw query.
   ⚠️ **singleflight (thundering-herd coalescing) deferred**: the cap already closes the
   OOM vector, thundering herd on a single-device LAN hub is low-risk, and a correct
   hand-rolled singleflight (no external deps allowed — pure-stdlib module) is more
   surface than this pass warranted. Revisit if open-meteo rate-limits in practice.
   (b) Chat sessions (`internal/chat/chat.go`): now a **256-session LRU cap**
   (`evictSessionsLocked`, evict oldest `updated`); the new session is stamped before
   the cap check so it is never its own victim.
3. ✅ **done — B5 sync path traversal** (`internal/notessync/notessync.go`): `putFile`/
   `listRemote` interpolated `req.Repo`/`name`/`req.Branch` unescaped; `safeName` was
   only on `/upload`. Now strict `validRepo` (owner/name, no `.`/`..` segment)
   replacing `Contains("/")`, `safeName` per-file in `runSync` (invalid → per-file
   error), and `url.PathEscape`(name)/`url.QueryEscape`(branch) on the GitHub URLs.
4. ✅ **done — B4 timeouts** (`cmd/lilyhub/main.go`): added `ReadTimeout` 60 s +
   `IdleTimeout` 120 s; `WriteTimeout` left off (chat path legitimately ≤60 s upstream,
   and `ReadTimeout` doesn't bound the handler once the body is read).
5. **B3 + A3** — one commit + serial round-trip per file per sync
   (`notessync.go:255-288`, `maxParallel` hardwired 1 at :98). Move to the Git Data
   API (blobs → tree → one commit → ref) — batches, removes the parent-ref race that
   forces serial, and makes A3's scaffolding either real or deletable.
6. **A1/A2 + cosmetics** — dead `/api/notes/list` (`notessync.go:106,388-412`; the
   firmware only calls `/upload` and `/sync`); stale `/healthz` comment
   (`main.go:38-39` — the device raw-TCP-connects, never HTTP-pings); `gofmt -w`;
   `truncate` duplicated in 3 packages (hoist); minor: telegram proxy silently
   truncates >1 MiB bodies (`telegram.go:81`); pin a `toolchain` in `go.mod`
   (declares go 1.22) for reproducible Pi builds.

Verified clean, don't re-audit: outbound timeouts/contexts all correct, no goroutine
leaks, no dropped response bodies, chat mid-flight snapshot pattern race-free (has a
test).

---

## E. Carried over from phase 1 (unchanged, do not lose)

- **Hardware smoke-test checklist** in `OPTIMIZATION_PROGRESS.md` — still pending; it
  gates trusting the phase-1 radio/audio/§2.13/§2.17 passes AND is where P2.1/P2.6
  verification should be added.
- **§1.5 write-only `monitor_params_t` fields** — removal deletes live I2C gauge
  reads; rides the hardware pass.
- **§3.1 font-picker cap** — now quantified as P2.11 item 3 (74.9 KB, not ~118 KB).
- **§2.16 HTTP double-buffer** — stays deferred (see P2.10 note).
- **§1.7 timezone-extern consolidation** — blocked on relocating timezone logic out
  of the UI layer; unchanged.

---

## Suggested execution order

1. ~~**P2.3** settings ping off-thread~~ ✅ done.
2. ~~**P2.4** cJSON→PSRAM hook~~ ✅ done.
3. ~~**P2.2** NFC lock gate, then **P2.1** keyboard fake-sleep gate~~ ✅ done (code) —
   ⚠️ both still owe the hardware smoke-test (now on the checklist); P2.1 also fixes
   the sleep-power story.
4. ~~**P2.8** minimal watchdog yield in the bulk storage/crypto loops (crash vector,
   ~1-line mitigation).~~ ✅ done (throttled flush+yield in the shared
   `storage_progress_cb`); full worker+drain for copy-to-hub still deferred.
5. ~~**P2.10 flash items 1+2** (montserrat_40 + decoders): −129 KB, low risk, one
   commit each.~~ ✅ done — **−102 KB measured** (70.5 % → 68.0 % flash), one commit each.
6. ~~**§4 server pass** (items D1–D4 are each small; D1 and D3 are security).~~ ✅ done
   — D1 SSRF, D2 map caps (singleflight deferred), D3 path traversal, D4 timeouts; four
   commits + tests. **D5 (Git Data API) and A1/A2 cosmetics remain.**
7. Then the product decisions (picker cap, mono face, GPS toggle, WiFi PS) and the
   optional deeper work (P2.7 file browser, P2.9 journal off-thread, D5 Git Data API).

Per repo convention: one `<code>` commit + one `<docs>` commit per item, update the
status table you'll inevitably add to this file, and keep
`OPTIMIZATION_PROGRESS.md`'s methodology warnings in force — **re-verify every
file:line here against current source before editing; this file goes stale the same
way the phase-1 report did.**
