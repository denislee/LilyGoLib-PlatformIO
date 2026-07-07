# Optimization Progress & Handoff

Companion to `OPTIMIZATION_REPORT.md` (the analysis). This file tracks **what has
been applied**, **what remains**, and — most importantly — **how to verify safely**
before removing anything else. Last updated **2026-07-06**.

`OPTIMIZATION_REPORT.md` is analysis only and is **stale relative to the code** (it
was generated 2026-07-05, before any of the changes below). Do **not** trust its
"dead"/"live" verdicts blindly — re-verify every symbol against current source. See
[Methodology](#methodology--read-this-before-removing-code) for two concrete cases
where the stale report would have caused a regression.

---

## Status by report section

| § | Item | Status |
|---|---|---|
| 1.1 | Whole dead files (`ui_power.cpp`, `test_sleep.cpp`, `keyboard_audio.h`) | ✅ done |
| 1.2 | ~60 never-called `hw_*` functions | ◐ partial — storage/display/power/wireless-BLE/4×system/sensors-peripherals/core-apps/UI-helpers+system-UI-chain/**audio** done; **radio remains** (HIGH-risk) |
| 1.3 | Stale declarations (no definition) | ✅ done (5 removed) |
| 1.4 | Dead `#ifdef` branches | ◐ partial — `USING_UART_BLE`, `USING_MAG_QMC5883`, `USING_BME280`, `USING_IR_REMOTE`/`_RECEIVER` gone; **rest remain** (incl. `USING_LED_INDICATOR` bug candidate) |
| 1.5 | Dead types/fields/globals | ☐ not started |
| 1.6 | LIKELY-dead (LVGL v8 theme branch, `HalResult`) | ☐ not started (needs judgement) |
| 1.7 | Redundant declarations | ☐ not started |
| 2.1 | Telegram poll/send off the LVGL thread | ✅ done |
| 2.2 | Hub config + reachability caching | ✅ done (earlier) |
| 2.3 | PBKDF2 key cache | ✅ done (earlier) |
| 2.4 | Chat voice-memo base64 streaming | ✅ done |
| 2.5 | Telegram internet probe off-thread | ✅ done (earlier) |
| 2.6 | Skip telegram re-render when unchanged | ✅ done |
| 2.7 | Audio-notes size from dir scan | ✅ done |
| 2.8 | Journal snippet cache shrink | ✅ done (earlier) |
| 2.9 | SSH scrollback length tracking | ✅ done |
| 2.10 | Home-screen ping off-thread | ✅ done (earlier) |
| 2.11 | O(n²) note-list merges → set | ✅ done |
| 2.12 | Status-bar 1 Hz RTC + gauge sweep | ☐ **needs care** (see below) |
| 2.13 | Audio stop busy-wait on UI thread | ☐ **needs care** |
| 2.14 | Home-app visibility NVS cache | ✅ done |
| 2.15 | MP3 decode buffer → PSRAM | ✅ done |
| 2.16 | `hw_http_request` double-buffer | ⊘ **deferred** (poor risk/reward — see below) |
| 2.17 | Settings blocking HTTPS inline | ☐ not started (user-initiated) |
| 2.18 | Consolidate duplicated helpers | ☐ not started (cleanup) |
| 2.19 | Telegram notif-toggle NVS cache | ✅ done |
| 3.1 | Font-picker trimming (~250–300 KB flash) | ◐ Montserrat 34–46 disabled; **picker-cap product decision remains** |
| 3.2 | Unused image sources | ✅ compiled `img_*.c` done; **25 PNG/JPG source orphans in `src/images/` remain** (not compiled, pure hygiene) |
| 3.3 | Drop 3 dead `lib_deps` | ✅ done |
| 3.4 | Disable LVGL demos/examples + unused fonts | ✅ done |
| 3.5 | Untrack `compile_commands.json` | ✅ done; **`lib/LilyGoLib/firmware/*.bin` (~96 MB) still tracked** |
| 4 | Go server (lilyhub) A1–B6 | ☐ not started (separate codebase, `server/`) |

Legend: ✅ done · ◐ partial · ⊘ deferred · ☐ not started

---

## Measurable results so far

- **Flash −48 KB**: removing the dead keyboard-tone MP3 blob (`6449afa`) —
  `firmware.bin` 3,004,921 → 2,956,189 B. This one was *not* free before removal: the
  unreachable `APP_EVENT_PLAY_KEY` case still referenced the blob, so `--gc-sections`
  couldn't strip it. Everything else in §1/§3 was already gc-stripped, so those
  deletions are build-time/hygiene wins, not flash.
- **Internal RAM −4.6 KB**: MP3 decode frame buffer moved to PSRAM (`f6d90ed`) —
  94,232 → 89,648 B used. This is the scarce heap WiFi/TLS contend for.
- Current image: RAM 27.4 % (89,656 B) · Flash 70.5 % (2,955,369 B).

The big *perceptual* wins are the §2.1/§2.4/§2.6 telegram+chat changes: the UI no
longer freezes ~1–2 s per poll or per voice-memo send.

---

## Completed commits (this effort)

Perf (§2): `3b373da` `46686f3` `932eed0` `ba2a4ae` `c22a83e` `e897fe1` `37d67e5`
`bc78d41` `c7ff2c2` `5c4ab5f` `590faf4` `9c60c5b` `c01a540` `f6d90ed`
Build/hygiene (§3): `ab01a78` `0089c55`
Dead code (§1): `e082021` `39a388a` `6449afa` `036d52a` `4db8948` `7fcda8d`
`409be67` `0e0e0f2`

New HAL primitive added along the way: `hw_read_sd_stream()` (`storage.h`/`storage.cpp`)
— chunked SD read that releases the SPI bus between chunks; used by the §2.4 voice-memo
streaming. It made `hw_get_file_size()` live again (was on the report's dead list).

---

## Methodology — READ THIS BEFORE REMOVING CODE

The report is stale. For every candidate function, verify against **current** source:

```bash
# A truly-dead function has ONLY its declaration (header) + definition (.cpp).
# Count references across all compiled sources:
grep -rIn "\bFUNC\b" src/ --include=*.cpp --include=*.c --include=*.h --include=*.ino | wc -l
# 2 == dead (decl + def).  >2 == inspect the extra lines: a call site => LIVE.
# Then confirm the extras are only in the owning HAL .h/.cpp, never an app/ui caller:
grep -rIn "\bFUNC\b" src/ --include=*.cpp --include=*.h --include=*.ino | sed 's/:.*//' | sort | uniq -c
```

**Two real catches this discipline made — both would have shipped a regression:**

1. **`hw_get_file_size`** — on the report's §1.2 dead list, but the §2.4 voice-memo
   work made it a live caller. Kept it.
2. **`hw_set_msc_prefer_sd`** — nearly removed because an ad-hoc filter
   `grep -v "storage.cpp"` also swallowed **`settings_storage.cpp`** (substring match!),
   hiding its one live caller. When excluding the owning file, anchor the path:
   `grep -vE "src/hal/storage\.(cpp|h):"`, not `grep -v "storage.cpp"`.

Other rules that held up:
- Functions are **interleaved** with live ones in the `.cpp` — remove each precisely,
  anchoring the edit on the *next live* function so the match stays unique.
- Watch `\b` word boundaries: `\bhw_get_preferred_txt_files\b` does **not** match
  `hw_get_preferred_txt_files_info` (the `_` after `files` is a word char), so the two
  are correctly distinguished — the base one is dead, `_info` is live (journal reconcile).
- When removing a function, also remove any now-orphaned helpers it *solely* used
  (e.g. the `lilygo_request_fake_sleep_toggle` extern), but keep shared helpers
  (`list_files`, `normalize_path`, `delete_path_recursive`, `<Esp.h>`).

**Always rebuild BOTH targets + tests after each domain** (per CLAUDE.md discipline):

```bash
pio run -e tlora_pager          # hardware
pio run -e emulator_lora_pager  # matching emulator (catches #else / stub drift)
pio test -e native_test         # 19 Unity cases
```

The emulator has already caught one hardware-clean mistake this session (an `#else`
branch still referencing a renamed parameter). Do not skip it.

---

## Remaining §1.2 — what's left, by risk

### LOW risk — continue the same sweep
- ~~**sensors/peripherals**~~ ✅ done — removed `hw_mag_enable`, `hw_mag_get_polar`
  (whole `USING_MAG_QMC5883` block), `hw_bme_enable`/`hw_bme_get_data` (whole
  `USING_BME280` block), and the entire IR block (`hw_set_remote_code`,
  `hw_get_remote_code`, `hw_ir_function_select`, `irsend`/`irrecv` + `USING_IR_*`).
  `hw_bme_enable` was also dead (no header decl, no caller) — removed with the block.
- ~~**core/apps**~~ ✅ done — removed `core::notify::dismiss` (+ its now-dead
  `s_dismissed` state and `pump()` handling), `hal::secret_erase`,
  `apps::home_apps_symbol`, `core::AppManager::getActiveApp`/`getApps`,
  `core::System::getMainScreen`/`getMenuPanel`.
- ~~**UI helpers**~~ ✅ done — removed `ui_create_option`, `create_switch`,
  `create_label`, `create_radius_button`, `create_back_button`,
  `ui_text_editor_new_document` (+ their decls in `ui/widgets.h`, `ui/back_button.h`,
  `ui_define.h`). `create_text`/`child_focus_cb` kept (shared with live builders).
- ~~**system UI chain**~~ ✅ done — removed the `ui_show_wifi_process_bar` →
  `ui_create_process_bar` chain (`ui_msg.cpp` + `ui_tools.cpp`), which cascaded into
  the now-orphaned `enable_input_devices`/`disable_input_devices` (`ui_tools.cpp`) and
  their sole callees `hw_enable_input_devices`/`hw_disable_input_devices`
  (`hal/system.cpp`/`.h`). Also removed `isinMenu` (both decls) which dragged
  `core::System::isInMenu()`. `ui_msg_pop_up` kept (many live callers).
  - ⚠️ **REPORT CORRECTION**: the report/§1.2 listed the `ui_lock`/`ui_unlock`
    "compat aliases" (`ui_main.cpp`) as dead — **they are NOT**. Compiled vendor
    `lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp` declares them `extern` and calls them
    (lines ~1309–1319) from the radio/power SPI-guard path. Removing them is a link
    error. **Kept.** (The `isinMenu` in `lib/.../examples/` is the vendor's stale,
    non-compiled tree — ignore it.)

### HIGH risk — do as a DEDICATED pass, ideally with a hardware smoke-test
- ~~**audio FFT subsystem**~~ ✅ done (compile + emulator verified; **not** yet
  hardware-smoke-tested — the mic/FFT path never runs, so risk is low). Removed
  `hw_set_mic_start`/`hw_set_mic_stop`/`hw_audio_get_fft_data` + `process_channel_fft` +
  all six PSRAM buffers + the `FFTData`/`FFT_SIZE`/`SAMPLE_RATE`/`FREQ_BANDS` types
  (`types.h`, §1.5) as one unit. Also dropped the now-orphaned `dsps_fft2r.h`/
  `dsps_wind_hann.h` esp-dsp includes, the orphaned `<math.h>` include, and the
  unused `FILESYSTEM` macro. The recording path (`hw_rec_*`, codec open/close) is
  independent and untouched.
- ~~**audio music-list chain**~~ ✅ done — removed `hw_get_filesystem_music` + its
  static chain `listDir`/`hw_fat_list`/`hw_sd_list` and `hw_set_sd_music_pause`/
  `hw_set_sd_music_resume`. `AudioParams_t`/`audio_source_type_t` kept (live in
  `hw_set_sd_music_play`/`hw_sd_play`).
- **radio** (`radio_common.cpp`, `radio.cpp`, `radio/nrf2401.cpp`, per-chip files):
  the biggest and riskiest. `hw_set_radio_listening`/`hw_set_radio_tx`/`hw_get_radio_rx`/
  `radio_transmit`; the whole NRF24 API except `hw_nrf24_begin`; `hw_set_usb_rf_switch`;
  the `radio_get_{freq,bandwidth,tx_power}_from_index` + `_length()` trios in all four
  chip files. Removing the TX/RX API shrinks the **ISR / FreeRTOS event-group plumbing**
  in `hw_radio_begin`, and lets the `hw_nrf24_begin` boot call (`system.cpp`) +
  `USING_EXTERN_NRF2401` block (`board_config.h`) go too. This touches interrupt and boot
  paths — a compile-clean mistake here can still brick radio/boot. **Do not rush blind.**

---

## Deferred / judgement-call items

- **§2.16 (`hw_http_request` double-buffer)** — deferred. The safe fix (stream
  `getStream()` into `out`) means hand-rolling a bounded read loop over the embedded TLS
  stream with timeout + chunked-encoding handling; a mistake truncates responses or hangs
  a fetch. The benefit is one transient copy of a small (~2–5 KB) body that frees
  immediately. Poor risk/reward vs. the robust `getString()`. Revisit only if profiling
  shows internal-heap pressure during TLS.
- **§2.12 (status-bar RTC + gauge sweep)** — the clock fix wants `time(nullptr)` +
  `localtime_r` instead of an I2C RTC read every second. **Blocker: confirm the RTC is not
  the post-deep-sleep source of truth** before switching, or the clock can show wrong time
  after wake. Needs the wake/time-seed flow understood (and ideally hardware).
- **§1.6 `HalResult`/`HalStatus`** — referenced only by `test/test_hal_result`, but
  CLAUDE.md calls it the *preferred* error type for new code. Keep unless that migration is
  being abandoned.
- **§1.4 `USING_LED_INDICATOR`** — not just dead code: it's a functional-bug candidate.
  Settings shows an LED-brightness slider, but `hw_set_led_backlight`'s body is compiled
  out, so the slider does nothing and `led_indicator_level` is persisted-but-never-applied.
  Decide: define the macro where the LED exists, or remove the slider.

---

## Repo-hygiene leftovers (safe, not yet done)

- `lib/LilyGoLib/firmware/*.bin` — six ~16 MB factory images (~96 MB) tracked in git (§3.5).
- `src/images/` — 25 of 32 PNG/JPG sources are orphans never generated into `.c` (§3.2).
- `test/test_desktop/test_main.cpp` — 26-line `2+2` placeholder (§3.5).
- Duplicated helpers to consolidate (§2.18): `url_encode` twice within `ui_weather.cpp`;
  `json_escape`/`b64_encode` duplicated between `ui_chat.cpp` and `hub.cpp`; NVS
  `load/save_pref` triplets re-implemented across ~5 files.

---

## Suggested next order

1. Finish the **LOW-risk §1.2** groups (sensors/peripherals, core/apps, UI helpers,
   system UI chain) — same verified sweep as above, one commit per domain.
2. **§1.5 / §1.7** — dead types/fields/globals and redundant declarations (many pair with
   the functions already removed).
3. Dedicated **audio-FFT** pass (move buffers + types + functions as a unit).
4. Dedicated **radio** pass — with a hardware smoke-test (TX/RX + boot) afterward.
5. Repo-hygiene: untrack `firmware/*.bin`, prune `src/images/` orphans.
6. **§4 Go server** (`server/`) — independent; A1/A3/B4/B5/B6 are the concrete items.
