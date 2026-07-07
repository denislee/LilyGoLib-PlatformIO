# Optimization & Dead-Code Report

Generated 2026-07-05 by a four-way analysis pass (unused C++ code, C++ performance,
assets/build config, Go server). Every finding below was verified against the actual
source and, where noted, against the linked `firmware.elf`/`firmware.map`
(firmware.bin = 2.86 MB). Nothing has been changed yet — this is analysis only.

> ⚠️ **This report is a point-in-time analysis and is now STALE.** Much of §1–§3
> has since been applied. See **`OPTIMIZATION_PROGRESS.md`** for what's done vs.
> remaining, the verification methodology, and corrections where this report was
> wrong — notably `ui_lock`/`ui_unlock` are **not** dead (compiled vendor code
> calls them; removing them is a link error). Re-verify every "dead" verdict here
> against current source before acting on it.

---

## Executive summary

| Area | Headline |
|---|---|
| Dead C++ code | **~6,200 removable lines** (~2,000 excluding one generated blob), incl. 2 whole dead files and ~60 never-called `hw_*` functions |
| Firmware perf | Telegram/chat/settings still run **blocking HTTPS + TCP probes on the LVGL thread**; PBKDF2 re-derived per encrypted read; several O(n²) merges |
| Flash size | Fonts are **665 KB = 22 % of the image**; ~250–300 KB recoverable with modest UX decisions. Everything else is already stripped by `--gc-sections` |
| Build/repo | LVGL demos+examples and 6 unused Montserrat sizes compile on every clean build; 3 dead `lib_deps`; 31 MB stale `compile_commands.json` tracked in git |
| Go server (lilyhub) | Structurally sound; one dead endpoint, dead concurrency scaffolding, a URL-escaping robustness gap, missing server body timeouts |

---

## 1. Unused / dead C++ code (`src/`)

Verification method: every symbol grepped across `src/`, `test/`, `factory.ino*`,
`main.cpp`; macro-generated references (`APP_FACTORY`, `LV_IMG_DECLARE`) and
string-based app switching checked separately. "2 refs" = declaration + definition
only, zero call sites. Matches inside `lib/LilyGoLib/examples/` are the vendor's
stale copy of this tree and are **not compiled** (`src_dir = src`).

### 1.1 Whole files that are dead — CERTAIN

| File | Size | Evidence |
|---|---|---|
| `src/audio/keyboard_audio.h` | 4,042 lines, ~48 KB PROGMEM | Only read in `audio.cpp:323` under `case APP_EVENT_PLAY_KEY:`; nothing ever posts `APP_EVENT_PLAY_KEY` (grep: enum def + case label only). Kills `playMP3()`/`mp3_fill_mem()` (`audio.cpp:162–181`) too. |
| `src/ui_power.cpp` | 197 lines | `ui_power_enter` has zero external references. Settings has its own power-off path (`ui_settings.cpp:335` → `hw_shutdown`). Its callee `hw_get_device_power_tips_string()` has **no ARDUINO definition** — hardware builds only link via `--gc-sections`. Deleting it also frees `img_poweroff.c` (289 KB source) and the `hw_get_device_power_tips_string` decl + emulator stub (`main.cpp:34`). |
| `test_sleep.cpp` (repo root) | — | Orphan scratch sketch, never compiled. |

### 1.2 Functions defined but never called — CERTAIN (~1,600 lines)

**hal/storage** (`storage.h` / `storage.cpp`, ~345 ln): `hw_save_file` (:245),
`hw_save_internal_file` (:299), `hw_delete_internal_file` (:358), `hw_get_file_size`
(:471), `hw_read_file_chunk` (:501), `hw_read_internal_file` (:612),
`hw_get_txt_files` (:681), `hw_get_storage_prefer_sd`/`hw_set_storage_prefer_sd`
(:879/:884), `hw_get_msc_prefer_sd` (:896), `hw_get_preferred_txt_files` (:1131),
`hw_get_sd_size` (:101).

**hal/audio** (~280 ln): `hw_get_filesystem_music` (:854) + its dead static chain
`listDir`/`hw_fat_list`/`hw_sd_list` (:790–853); `hw_set_sd_music_pause`/`_resume`
(:912/:920); the **entire FFT/spectrum subsystem** — `hw_set_mic_start`/`hw_set_mic_stop`/
`hw_audio_get_fft_data` (:441/:484/:412) plus `process_channel_fft` and buffers
(:355–505); `FFTData`, `FFT_SIZE`, `SAMPLE_RATE`, `FREQ_BANDS` (`types.h:266–274`).

**hal/radio** (~370 ln): `hw_set_radio_listening`, `hw_set_radio_tx`,
`hw_get_radio_rx`, `radio_transmit` (`radio_common.cpp:82–176` — comment at :166
even says "Currently unused"); the whole NRF24 API except `hw_nrf24_begin`
(`radio/nrf2401.cpp`: `hw_has_nrf24`, `hw_get/set_nrf24_params`,
`hw_set_nrf24_listening`, `hw_clear_nrf24_flag`, `hw_set_nrf24_tx`, `hw_get_nrf24_rx`
— ~175 of 227 ln); `hw_set_usb_rf_switch` (`radio.cpp:29`); the
`radio_get_{freq,bandwidth,tx_power}_from_index` + `_length()` trios in all four chip
files (~100 ln). If the radio TX/RX API goes, the ISR/event-group plumbing in
`hw_radio_begin` (`radio_common.cpp:23–37`) shrinks to nothing, and the
`hw_nrf24_begin` boot call (`system.cpp:218`) + `USING_EXTERN_NRF2401` block
(`board_config.h:15–17`) should go with it.

**hal/wireless** (~60 ln): `hw_enable_ble`/`hw_deinit_ble`/`hw_disable_ble`/
`hw_get_ble_message` (:569–589 — doubly dead: bodies need never-defined
`USING_UART_BLE`), `hw_set_ble_kb_key`/`_release` (:645/:656),
`hw_wifi_get_saved_ssid` (:279).

**hal/display** (~52 ln): `hw_get_disp_backlight`, `hw_get_disp_is_on`,
`hw_get_disp_timeout_ms`, `hw_inc_brightness`, `hw_dec_brightness`,
`hw_get_kb_backlight`, `hw_flush_keyboard`.

**hal/system** (~43 ln): `hw_fake_sleep_toggle` (:697), `hw_print_mem_info` (:733),
`hw_get_variant_name` (:425), `hw_set_cpu_freq` (:707); `ui_show_wifi_process_bar`
(defined `ui_msg.cpp:98`, zero callers → `ui_msg.cpp:41–110` dead, which was the only
caller of `ui_create_process_bar` in `ui_tools.cpp:26–90` — dead chain, ~135 ln);
`isinMenu` (`ui_main.cpp:78`, declared twice — `hal/system.h:80` and
`core/system_hooks.h:56`) → drags `core::System::isInMenu()` with it;
`ui_lock`/`ui_unlock` compat aliases (`ui_main.cpp:75–76`).

**hal/power** (~35 ln): `hw_get_battery_history`, `hw_get_charger_current`,
`hw_get_min/max_charge_current`, `hw_get_charge_steps`.

**hal/sensors + peripherals** (~115 ln): `hw_mag_enable`/`hw_mag_get_polar` (need
never-defined `USING_MAG_QMC5883`), `hw_bme_get_data` (never-defined `USING_BME280`),
the whole IR block `peripherals.cpp:180–232` (`hw_set_remote_code`,
`hw_get_remote_code`, `hw_ir_function_select`, `irsend`/`irrecv` globals).

**UI layer** (~230 ln): `ui_create_option` (`ui_tools.cpp:92`), `create_switch`
(:656), `create_label` (:702), `create_radius_button` (:752), `create_back_button`
(:767 — `ui_show_back_button` doesn't use it), `ui_text_editor_new_document`
(`ui_text_editor.cpp:515`).

**core / apps** (~50 ln): `core::notify::dismiss` (`notify.cpp:160`),
`hal::secret_erase` (`secrets.cpp:98`), `apps::home_apps_symbol` (`menu_app.cpp:888`),
`core::AppManager::getActiveApp`/`getApps`, `core::System::getMainScreen`/`getMenuPanel`.

### 1.3 Stale declarations with no definition — CERTAIN

Calling any of these is a link error today: `hw_check_and_migrate_storage`
(`storage.h:107`), `hw_set_trackball_callback`/`hw_set_button_callback`
(`system.h:74–75`), `hw_set_audio_effect_3d`/`hw_set_audio_effect_ab_class`
(`audio.h:26–27`).

### 1.4 Dead `#ifdef` branches — CERTAIN (macro defined nowhere)

`POLLING` (`nfc_reader.cpp:123–142`), `ARDUINO_T_DECK_V2` (`display.cpp:96,116`,
`lr1121.cpp:46`), `USING_UART_BLE`, `USING_IR_RECEIVER`, `USING_TRACKBALL`
(`system.cpp:770` + `trackballEventCallback`), `USING_MAG_QMC5883`, `USING_BME280`,
`USING_SI473X_RADIO`, `USING_QMI8658_SENSOR`, `RADIO_FIXED_FREQUENCY`.

⚠️ **`USING_LED_INDICATOR` is a functional-bug candidate, not just dead code**:
Settings shows an LED-brightness slider when `hw_has_indicator_led()`, but
`hw_set_led_backlight`'s body is compiled out (`display.cpp:55`), so the slider does
nothing and `led_indicator_level` is persisted but never applied. Either define the
macro where the LED exists or remove the slider.

### 1.5 Dead types, fields, globals — CERTAIN

- `event_define.h:19–55`: entire `USING_ST25R3916` block (`nfcData_t`, `app_event_t`,
  `app_audio_play_t`, `ndefType*`) unused; enum values `APP_EVENT_PLAY_KEY`,
  `APP_EVENT_RECOVER`, `APP_NFC_EVENT`.
- `types.h`: `hw_trackball_dir` (:258), `keyboard_type_t` + `DEVICE_KEYBOARD_TYPE`
  (:32, `board_config.h:32`), `monitor_params_type_t` + 13 write-only
  `monitor_params_t` fields (:163–185 — written in `power.cpp:285–368`, read by
  nobody; consumers use only `battery_percent`/`is_charging`/`temperature`; the
  matching gauge-register reads in `power.cpp` become removable — see perf §2.12),
  `wifi_scan_params_t.bssid` (:141).
- `NFC_TIPS_STRING` (3 defines in `board_config.h`, zero uses); `RTC_DATA_ATTR`
  emulator shim (`ui_define.h:22`).
- Globals assigned once, never read: `main_screen`, `menu_panel`, `app_panel`,
  `app_g` (`ui_define.h:37–41`, `core/system.cpp:20–24`). Only `menu_g` is read.

### 1.6 LIKELY (verify before removing)

- `ui_theme.cpp:84–260` (~177 ln) — LVGL v8 `#else` theme branch; every env pins
  LVGL 9.x. Same for `ui_define.h:68–75` compat macros.
- `HalResult<T>`/`HalStatus` (`hal/result.h:78–104`) — referenced only by
  `test/test_hal_result`. **CLAUDE.md says this is the preferred error type for new
  code**, so keep unless that migration is being abandoned; `HalError`/
  `hal_error_string` are genuinely used.

### 1.7 Redundant declarations

`isinMenu` declared in two headers; `notes_crypto_path_is_protected` declared in both
`notes_crypto.h:30` and `notes_path.h:19`; `tg_get_unread_count` re-declared locally
in `menu_glance.cpp:23`; ad-hoc externs in `factory.ino:21–29` duplicating headers
(`timezone_get_user_posix` has no header declaration at all).

---

## 2. Firmware optimization opportunities

### HIGH impact

**2.1 Telegram polls with blocking HTTPS on the LVGL thread** —
`ui_telegram.cpp:780–812` (`poll_tick`) runs `fetch_chats`/`fetch_messages`
synchronously every 5–10 s (each a full HTTPS round-trip, 5 s + 5 s timeouts);
`fetch_messages` then calls `mark_chat_read` — a second blocking POST in the same
tick. `send_text` (Enter handler) is also synchronous + an immediate refetch = up to
3 sequential HTTPS calls per keystroke. The UI freezes ~1–2 s every poll while a chat
is open. **Fix**: the correct pattern already exists twice in the codebase —
`tg_bg_task` (same file, :1349) and the weather worker+drain-timer
(`ui_weather.cpp:878–955`). Move fetch/send/mark-read onto the worker. *Risk:
needs care (onStop teardown).*

**2.2 Hub reachability probe: 1.5 s blocking TCP connect + 3–4 NVS opens per
request** — `tg_http_request` (`ui_telegram.cpp:442`) calls `hal::hub_is_enabled()`
(2 NVS opens) then `hub_is_reachable()` (2 more opens + raw `WiFiClient::connect`
with 1.5 s timeout, `hub.cpp:225–258`) on every poll/send/mark-read, on the LVGL
thread. Hub configured-but-down ⇒ every telegram op eats +1.5 s. `hub_get_url()`
(3 NVS opens) is also hit per fetch in weather and chat. **Fix**: cache
enabled/URL in RAM statics invalidated by `hub_set_*`; cache the reachability
verdict with a TTL — the status bar already probes this every ~10 s in a background
task (`core/system.cpp:297–325`); expose that result as `hal::hub_last_reachable()`.
*Risk: safe.*

**2.3 PBKDF2 (10k iterations) re-derived on every encrypted read** —
`notes_crypto.cpp:166–201` (`derive_key_iv`) costs tens–100+ ms per call; the
prewarm slot is encrypt-only/single-shot, so **reads always pay full PBKDF2**. The
journal reconcile pass decrypts a snippet of every changed note on the LVGL thread
(`ui_journal.cpp:230–262`), multiplying this. **Fix**: small LRU `salt → (key,iv)`
cache bound to the passphrase generation, guarded by the existing `prewarm_mutex()`,
zeroized in `notes_crypto_lock()`. *Risk: needs care (key-material lifetime).*

**2.4 Chat voice memo: whole WAV + base64 in RAM, 3 concurrent copies, on the UI
thread** — `ui_chat.cpp:681–703` reads the entire WAV, base64-encodes (+33 %), then
`kick_off_send` copies again. 1 min audio ≈ 7 MB peak; the 5-min cap
(`HW_REC_MAX_MS`) cannot fit in PSRAM at all. **Fix**: move read+encode into the
existing `send_task` and stream in ~3 KB chunks (multiples of 3 bytes) straight into
`ctx->body`. *Risk: needs care.*

**2.5 `internet_available()` blocks LVGL 1.5 s, re-armed every 5 s while offline** —
`ui_telegram.cpp:407–427`: TCP probe to 1.1.1.1:53, failure cached only 5 s. WiFi-up/
internet-down ⇒ periodic 1.5 s jank forever. **Fix**: fold the probe into the
background fetch task; read only the cached verdict on the UI thread. *Risk: safe.*

### MEDIUM impact

**2.6 Telegram rebuilds the full widget tree every poll even when nothing changed**
— `render_msgs` (:597–667) / `render_chats` (:507–540) do `lv_obj_clean` + recreate
~20×3–4 objects + `ascii_safe()` UTF-8 walks per label. **Fix**: compare a cheap
signature (newest msg id + count; per-chat `(id, unread)` hash) and skip — the
journal already does exactly this (`entries_signature`, `ui_journal.cpp:379`).

**2.7 Audio-notes list re-opens every WAV just to read its size** —
`ui_audio_notes.cpp:134–165`; `hw_list_sd_entries` already returns `HwDirEntry::size`
(`storage.cpp:782`). **Fix**: `n.data_bytes = r.size >= 44 ? r.size - 44 : 0;`,
delete the SD-reopen block. One line.

**2.8 Journal holds 4 KB plaintext snippets for all notes forever + per-render
copy** — `kSnippetBytes = 4096` (`ui_journal.cpp:37`), static vector never cleared;
100+ notes ⇒ ~400 KB of small `std::string`s landing in **internal** heap (squeezes
WiFi/TLS). `:823` copies the snippet before `lv_label_set_text` copies it again.
**Fix**: pass `entry.snippet.c_str()` directly; cut snippet size to ~1–1.5 KB (also
shrinks the §2.3 decrypt cost), or back the storage with PSRAM.

**2.9 SSH terminal copies the whole textarea buffer per output chunk** —
`ui_ssh.cpp:887–905`: `lv_textarea_get_text` → heap copy of up to 8 KB, 10×/s during
bursts, just to check length. **Fix**: track `term_len_` incrementally; only
fetch/trim when over `kTermMax`.

**2.10 Home-screen ping freezes the UI up to 3 s** — `menu_app.cpp:204–235` calls
`hw_ping_internet(..., 3000, ...)` synchronously (the comment admits it, using
`lv_refr_now` to show the tint first). **Fix**: one-shot task + result via drain
timer, same as the status-bar hub probe.

**2.11 O(n²) note-list merges** — `hw_get_preferred_txt_files_info`
(`storage.cpp:1262–1275`, feeds every journal reconcile), `hw_get_preferred_txt_files`
(:1148), `storage_bulk.cpp:58–75` dedupe: nested linear `std::string` compares
(200+200 notes ⇒ 40k compares). **Fix**: build a `std::set`/sorted vector of internal
names once, binary-search per SD entry.

**2.12 Status bar 1 Hz tick reads RTC over I2C + full gauge register sweep every
second** — `core/system.cpp:190–430` under the instance mutex (contended by the
10 ms keyboard task): `hw_get_date_time` hits the RTC although NTP + boot-seed keep
system time fresh, and `hw_get_monitor_params` (`power.cpp:269–360`) reads ~12 gauge
registers when the bar shows only percent + charging. `menu_glance.cpp` duplicates
the pair on its own 1 s timer. **Fix**: `time(nullptr)`+`localtime_r` for the clock
label; a lightweight `hw_get_battery_basic()` or 5 s cache TTL when not charging.
Ties into §1.5 — the 13 write-only `monitor_params_t` fields mean most of that
register sweep is producing data nobody reads. *Risk: needs care (confirm RTC is not
the post-wake source of truth).*

**2.13 Audio stop paths busy-wait on the UI thread** — `hw_rec_stop`
(`audio.cpp:713–722`) spins `while (recorder_running) delay(5)` through the final SD
write + WAV header rewrite; `hw_set_sd_music_play` similar. **Fix**: add a
`REC_STOPPED`/`PLAYER_STOPPED` event-group bit + bounded `xEventGroupWaitBits`, or go
fully async and let the existing tick timers observe completion.

### LOW impact (quick wins)

- **2.14** `home_apps_is_visible` opens NVS per tile, up to 14 `begin/end` cycles per
  menu rebuild (`menu_app.cpp:683–735, 893–906`) → one bitmask read, cached.
- **2.15** Static 4.6 KB MP3 frame buffer in internal BSS forever (`audio.cpp:42`) →
  PSRAM-alloc inside `play_mp3_with_filler` (its 16 KB refill buffer is already PSRAM).
- **2.16** `hw_http_request` double-buffers every response
  (`wireless.cpp:534–536`: `getString()` + `append`) → stream `getStream()` into
  `out` with `reserve(getSize())`.
- **2.17** Settings subpages run blocking HTTPS inline (favorites fetch
  `settings_telegram.cpp:235–260`, city search) — user-initiated, but back button is
  dead for the duration.
- **2.18** Duplicated helpers to consolidate: `url_encode` twice *within*
  `ui_weather.cpp` (:603 vs :1277); `json_escape` (`ui_chat.cpp:130` ≡ `hub.cpp:165`);
  `b64_encode` (`ui_chat.cpp:116` ≡ `hub.cpp:152`); UTF-8 sanitizer skeletons in
  weather + telegram; NVS `load/save_pref` triplets re-implemented in 5 files.
- **2.19** `fire_notifications` reads 2 NVS prefs per notification burst
  (`ui_telegram.cpp:1319–1346`) → statics invalidated by `tg_cfg_set_notif_*`.

**Checked and deliberately not flagged**: task stack sizes (all justified),
keyboard/rotary/NFC/charge task cadences, the recent journal/FFat/save-on-exit work,
`getSketchMD5` (cached upstream), font-getter struct copies.

---

## 3. Flash, assets, build config

Ground truth: `firmware.map` confirms `--gc-sections` already strips everything
unreferenced — so deleting dead sources saves **build time and repo hygiene, not
flash**, with the exceptions below.

### 3.1 Fonts — 665 KB (22 % of the image), all reachable via the font picker

No font is dead: all 22 custom arrays are reachable through `pick_font()`
(`ui_tools.cpp:988`) driven by Settings → Fonts. Measured (map-verified) costs:
Montserrat 10–32 296 KB, Montserrat 48 94.5 KB, **Montserrat 40 68.5 KB (single call
site in `core/system.cpp` — cheapest big win: reuse 48)**, Atkinson 53 KB, Inter
48 KB, Emoji 42 KB, JBMono 31 KB, Courier 27 KB, Unscii 4 KB. Realistic savings with
product decisions: drop montserrat_40 (+68.5 KB), cap picker sizes at 24
(+~118 KB), drop 1–2 least-used faces (+27–53 KB each) ⇒ **~250–300 KB**.

### 3.2 Images — all 7 `img_*.c` contribute 0 flash

`img_configuration/cry/dog/keyboard/nfc_bg/track` are unreferenced;
`img_poweroff` is referenced only by dead `ui_power.cpp`. All stripped by the linker.
Deleting them removes **~832 KB of source compiled on every build** (img_poweroff.c
alone is 289 KB). `src/images/`: 25 of 32 PNG/JPG sources are orphans (never
generated into .c). `src/audio/keyboard_audio.mp3` orphaned alongside its dead header.

### 3.3 platformio.ini

- **Removable lib_deps** (in `[env_arduino]`, all envs): `ESP8266Audio` (no header
  included anywhere — MP3 uses vendored `lib/libhelix-mp3`), `ESP32-BLE-Mouse-fork`
  (`BleMouse` never referenced), `ESP32 BLE Arduino` (Bluedroid; the keyboard fork
  hardcodes `USE_NIMBLE`, wireless.cpp uses NimBLE). LDF never links them ⇒ 0 flash,
  but install/scan waste. Also `lib_ignore` candidates: Adafruit SH110X + GFX dragged
  in transitively by TCA8418's library.properties.
- Version skew worth resolving: device LVGL `^9.4.0` (9.5.0 installed) vs emulator
  pinned `9.2.2`; RadioLib ini `^7.4.0` vs LilyGoLib's library.json wanting 7.1.2;
  emulator pulls LilyGoLib from GitHub while `lib/LilyGoLib` is vendored (divergence
  risk).
- ~120 lines of commented-out `src_dir` switches; emulator envs carry a no-op
  `lib_deps = ${env_emulator.lib_deps}` and `-D LV_USE_DEMO_WIDGETS=1`.
- `CORE_DEBUG_LEVEL=0` — good; default `-Os`; no LTO (normal for this platform).

### 3.4 lv_conf.h (`lib/LilyGoLib/src/lv_conf.h`)

- Montserrat **34/36/38/42/44/46 enabled but unreferenced** — compiled then
  stripped every build. Set to 0.
- **`LV_BUILD_EXAMPLES 1`, `LV_USE_DEMO_WIDGETS 1`, `LV_USE_DEMO_BENCHMARK 1`** —
  zero uses in device code; LVGL's examples+demos trees (hundreds of files) compile
  on every clean device build. Disabling is the single biggest build-time win.
- Unused-but-enabled widgets (calendar, canvas, chart, table, span, gif, qrcode,
  keyboard, win, animimg, scale, arc, led) — gc'd, compile-time only.

### 3.5 Repo hygiene

- `compile_commands.json` — **31 MB, stale (April), tracked in git**. Untrack.
- `lib/LilyGoLib/firmware/*.bin` — six 16 MB factory images (~96 MB) tracked.
- `test/test_desktop/test_main.cpp` — 26-line `2+2` boilerplate placeholder.

---

## 4. Go server (`server/`, lilyhub)

`go vet` clean; no leaked bodies, no per-request clients, no mutex-across-network,
cache TTL logic correct (checked explicitly). Findings:

- **A1. Dead endpoint** `/api/notes/list` (`notessync.go:106, 384–412`): firmware
  only calls `/api/notes/upload` and `/api/notes/sync`. Delete (with the `sort`
  import) or document as a curl-debug endpoint.
- **A2. Stale comment** on `/healthz` (`main.go:38–42`) claims the device pings it;
  the device actually raw-TCP-connects (`hub.h:43`). Fix the comment, keep the
  endpoint.
- **A3. Dead concurrency scaffolding** in `runSync` (`notessync.go:172–199`):
  semaphore+WaitGroup+mutex with `maxParallel` hardwired to 1 (and per its own
  comment it can never be raised under the Contents API). Replace with a plain loop.
- **B5. Robustness: sync path doesn't sanitize/escape file names**
  (`notessync.go:267`): `safeName` is applied only in `upload`; names with
  `?`, `#`, `/`, spaces mangle the GitHub URL or escape `notes/`. Run `SyncFile.Name`
  through `safeName` + `url.PathEscape`.
- **B3. One commit + one serial round-trip per file per sync**: the Git Data API
  (blobs → tree → commit → ref) would make big first-time syncs parallel + one
  commit, and legitimately removes the "must stay serial" constraint. Optional.
- **B4. Server missing body/idle timeouts** (`main.go:49–53`): only
  `ReadHeaderTimeout` is set while endpoints accept 8 MiB bodies — a stalled client
  parks a goroutine forever. Add `ReadTimeout`/`IdleTimeout` (a global `WriteTimeout`
  is unsafe for the 60 s×3-retry chat path — use `TimeoutHandler` per route if
  needed).
- **B6. Telegram proxy** (`telegram.go:47–48, 86–89`): forwards to any `http*` URL
  with the caller's bearer token (SSRF-shaped; LAN-only mitigates) and flattens
  Telegram's structured JSON errors into text 502s. Allowlist
  `https://api.telegram.org/`; pass error bodies through.
- **B1/B2. Cache**: no request coalescing on miss (open-meteo asks ≤1 req/min) and
  no entry-count cap between sweeps; forecast cache keys use the verbatim query
  string (param order/case creates duplicate entries). Low priority at
  single-device scale.
- **Cosmetics**: `truncate` duplicated in 3 packages; `gofmt -l` flags
  `chat.go`, `notessync.go`, `telegram.go`.

---

## 5. Suggested action order

1. **Zero-risk deletions** (§1.1–1.5, §3.2, §3.5): dead files, never-called
   functions, stale declarations, dead ifdef branches, unused image sources,
   `compile_commands.json`. ~6k lines / ~1.7 MB of source out of every build.
2. **Build-time wins** (§3.3–3.4): disable LVGL demos/examples + Montserrat 34–46,
   drop 3 dead lib_deps.
3. **UI-thread unblocking** (§2.1, 2.2, 2.5, 2.10): telegram worker migration + hub
   probe caching — the largest perceptible responsiveness wins.
4. **Cheap medium wins** (§2.7 one-liner, 2.9, 2.11, 2.14, 2.16, 2.19).
5. **Decide**: font-picker trimming for ~250–300 KB flash (§3.1); PBKDF2 key cache
   (§2.3, needs key-lifetime care); chat voice-memo streaming (§2.4); the
   `USING_LED_INDICATOR` bug-or-dead-feature call (§1.4); lilyhub robustness items
   (B4/B5/B6).
