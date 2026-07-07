# Optimization Progress & Handoff

Companion to `OPTIMIZATION_REPORT.md` (the analysis). This file tracks **what has
been applied**, **what remains**, and — most importantly — **how to verify safely**
before removing anything else. Last updated **2026-07-07** (§2.12 clock + gauge-TTL done;
§2.18 encoder consolidation done).

> **Milestone: the entire §1 dead-code audit (§1.1–§1.7) and the §3 repo-hygiene are
> complete.** §1.1 dead files, §1.2 ~60 never-called `hw_*` functions, §1.3 stale decls,
> §1.4 dead `#ifdef` branches (+ removed the phantom LED slider), §1.5 dead
> types/globals, §1.6 the LVGL v8 theme branch, §1.7 redundant decls; §3 untracked the
> ~96 MB `firmware/*.bin` and pruned all `src/images/` orphans. Deliberate leftovers, each
> with a documented reason in [Deferred](#deferred--judgement-call-items): the write-only
> `monitor_params_t` gauge fields (§1.5 → rides with the §2.12 gauge sweep, since removing
> them removes live per-second I2C reads); the `hw_devices[]` name-table slots and
> non-compiled chip-file branches (§1.4, intentional structure); scattered v9 guards and
> `HalResult` (§1.6, kept). **What's left is the remaining §2 perf items and the §4 Go
> server.** See [Suggested next order](#suggested-next-order).
>
> ⚠️ **Two removals are compile+emulator-verified only, NOT hardware-tested** — the
> **radio** and **audio-FFT** passes. Before trusting them on a device, run the
> [Hardware smoke-test checklist](#hardware-smoke-test-checklist-do-before-trusting-the-radio--audio-fft-passes).

`OPTIMIZATION_REPORT.md` is analysis only and is **stale relative to the code** (it
was generated 2026-07-05, before any of the changes below). Do **not** trust its
"dead"/"live" verdicts blindly — re-verify every symbol against current source. See
[Methodology](#methodology--read-this-before-removing-code) for three concrete cases
where the stale report would have caused a regression (incl. one it got outright
wrong: `ui_lock`/`ui_unlock`).

---

## Status by report section

| § | Item | Status |
|---|---|---|
| 1.1 | Whole dead files (`ui_power.cpp`, `test_sleep.cpp`, `keyboard_audio.h`) | ✅ done |
| 1.2 | ~60 never-called `hw_*` functions | ✅ done — all groups removed (storage/display/power/wireless-BLE/system/sensors-peripherals/core-apps/UI-helpers+system-UI-chain/audio/radio). Radio done conservatively: dead API removed, both live boot ISRs kept intact; non-compiled chip trios left as dead source (see radio note below). |
| 1.3 | Stale declarations (no definition) | ✅ done (5 removed) |
| 1.4 | Dead `#ifdef` branches | ✅ mostly done — earlier side effects of §1.2 (`USING_UART_BLE`, `USING_MAG_QMC5883`, `USING_BME280`, `USING_IR_REMOTE`/`_RECEIVER`, `RADIO_FIXED_FREQUENCY`), plus this session `POLLING` (2 blocks in `nfc_reader.cpp`, kept the live `#else`), `ARDUINO_T_DECK_V2` (`display.cpp` keyboard enable/disable), `USING_TRACKBALL` (whole self-contained block + type aliases in `system.cpp`), and **`USING_LED_INDICATOR` — resolved by REMOVING the phantom slider** (user decision): dropped the LED slider + `led_brightness_cb` (`settings_display.cpp`), `hw_set_led_backlight`/`hw_has_indicator_led` (`display.{cpp,h}`), `user_setting_params_t.led_indicator_level` + `HW_LED_INDIC_ONLINE` (`types.h`), and bumped `SETTINGS_VERSION` 10→11. **Left by design:** `USING_SI473X_RADIO`/`USING_QMI8658_SENSOR` are positional `hw_devices[]` name-table slots where `#ifdef NAME #else ""` is intentional self-documenting structure (see Deferred); `ARDUINO_T_DECK_V2` in the non-compiled `lr1121.cpp` (chip-file discipline). |
| 1.5 | Dead types/fields/globals | ◐ mostly done — FFT types (audio pass) + this session `hw_trackball_dir`, `keyboard_type_t`/`DEVICE_KEYBOARD_TYPE`, `event_define.h` NFC block (`nfcData_t`/`app_event_t`/`app_audio_play_t`/`ndefType*` + dead enum values `APP_EVENT_PLAY_KEY`/`APP_NFC_EVENT`), `NFC_TIPS_STRING`, `RTC_DATA_ATTR` shim, once-assigned `main_screen`/`menu_panel`/`app_panel`/`app_g` globals, `wifi_scan_params_t.bssid`. **Only remaining: the 14 write-only `monitor_params_t` gauge/voltage fields + `monitor_params_type_t`/`type` — now decoupled from §2.12 (which is done): §2.12 only *throttled* the sweep (1 s→5 s idle), it did not trim fields. Removing them still removes live I2C gauge reads, so it wants a device to validate — deferred to the hardware pass.** |
| 1.6 | LIKELY-dead (LVGL v8 theme branch, `HalResult`) | ✅ done — dropped the dead LVGL v8 `#else` theme branch in `ui_theme.cpp` (261→80 lines) and the v8 `#else`/empty-`#if v8` compat blocks in `ui_define.h` (v9 rename shims kept unconditionally). Every env pins LVGL 9.x (hw 9.4.0 / emu 9.2.2) so the v8 paths were already never compiled. **`HalResult`/`HalStatus` KEPT** (CLAUDE.md's preferred error type). Generated font `.c` files' `#if LVGL_VERSION_MAJOR==8` blocks left as-is (converter output). |
| 1.7 | Redundant declarations | ◐ mostly done — `isinMenu` (already gone via §1.2), `notes_crypto_path_is_protected` (kept the owning decl in `notes_path.h`, dropped the dup in `notes_crypto.h`, added `notes_path.h` include to `ui_file_browser.cpp`), `tg_get_unread_count` (menu_glance.cpp now includes `app_registry.h` instead of re-declaring). **Remaining: factory.ino timezone externs — deferred (see below).** |
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
| 2.12 | Status-bar 1 Hz RTC + gauge sweep | ✅ done — blocker resolved by analysis (no deep-sleep-reboot path; `hw_init` seeds the system clock from RTC at boot, NTP/GPS/manual keep both in step, fake/light sleep preserves it). Clock label now reads the system clock via new `hw_get_wall_clock` (`system.{h,cpp}`) instead of an I2C RTC read every second — routed the two 1 Hz display ticks (`core/system.cpp` status bar, `menu_glance.cpp`). Gauge sweep TTL extended 1 s→5 s when not charging (`power.cpp`). **§1.5 write-only-field removal NOT done here** — kept (still rides the hardware pass; the sweep is just throttled, not trimmed). |
| 2.13 | Audio stop busy-wait on UI thread | ☐ **needs care** |
| 2.14 | Home-app visibility NVS cache | ✅ done |
| 2.15 | MP3 decode buffer → PSRAM | ✅ done |
| 2.16 | `hw_http_request` double-buffer | ⊘ **deferred** (poor risk/reward — see below) |
| 2.17 | Settings blocking HTTPS inline | ☐ not started (user-initiated) |
| 2.18 | Consolidate duplicated helpers | ◐ partial — **encoders done**: `json_escape` (was 3× — ui_chat, ui_notes_sync, hub), `b64_encode` (3×), `url_encode` (2× within ui_weather) hoisted into new `hal::str_encode` (`str_encode.{h,cpp}`); all call sites routed through it; orphaned `<mbedtls/base64.h>` includes dropped. **Remaining**: NVS `load/save_pref` triplets across ~5 files (persistence — own careful pass) and the weather/telegram UTF-8 sanitizers (**left by design** — shared decode skeleton but different downstream logic: `sanitize_ascii` drops symbols, `ascii_safe` checks font glyph coverage). |
| 2.19 | Telegram notif-toggle NVS cache | ✅ done |
| 3.1 | Font-picker trimming (~250–300 KB flash) | ◐ Montserrat 34–46 disabled; **picker-cap product decision remains** |
| 3.2 | Unused image sources | ✅ done — removed **all 32** `src/images/*.png|jpg` (verified: zero `LV_IMG_DECLARE`/`&img_`/`lv_image_dsc_t` refs in compiled `src/`; no build/script references the dir; every file is recoverable from the vendor `examples/factory/images/` tree). Report said "25"; the earlier compiled-`img_*.c` removal had already orphaned the other 7. |
| 3.3 | Drop 3 dead `lib_deps` | ✅ done |
| 3.4 | Disable LVGL demos/examples + unused fonts | ✅ done |
| 3.5 | Untrack `compile_commands.json` | ✅ done; **`lib/LilyGoLib/firmware/*.bin` (6 × 16 MB) now untracked** (`git rm --cached` + `.gitignore`; files kept on disk). ⚠️ still present in git *history* — a full clone is unchanged until a history rewrite (filter-repo/BFG) is run. |
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
- Current image: RAM 27.4 % (89,624 B) · Flash 70.4 % (2,953,489 B) — **essentially
  unchanged across the whole §1 audit**: everything removed was already
  `--gc-sections`-stripped (or an unreferenced macro / type / `#ifdef` branch contributing
  0 bytes), so those deletions are build-time + source-hygiene wins, not flash/RAM. The one
  real flash win in this effort is still the MP3-blob removal above.
- **Source reduction this session ≈ 1,550 lines**: ~1,200 (§1.2 — sensors/peripherals,
  core/apps, UI helpers, audio, radio) + ~90 (§1.5/§1.7 dead types/globals + redundant
  decls) + ~85 (§1.4 dead `#ifdef` branches + LED slider) + ~180 (§1.6 LVGL v8 theme
  branch). Repo tree also lighter: 32 orphan images deleted, ~96 MB of `firmware/*.bin`
  untracked (§3, still in history).

The big *perceptual* wins are the §2.1/§2.4/§2.6 telegram+chat changes: the UI no
longer freezes ~1–2 s per poll or per voice-memo send.

---

## Completed commits (this effort)

Perf (§2): `3b373da` `46686f3` `932eed0` `ba2a4ae` `c22a83e` `e897fe1` `37d67e5`
`bc78d41` `c7ff2c2` `5c4ab5f` `590faf4` `9c60c5b` `c01a540` `f6d90ed` `e456232` `c7868e4`

- **§2.12** status-bar RTC + gauge sweep (`e456232` / `54c13e8`): new
  `hw_get_wall_clock()` (`system.{h,cpp}`) drives the two 1 Hz display ticks
  (`core/system.cpp`, `menu_glance.cpp`) off the ESP32 system clock instead of a
  per-second I2C RTC read; `hw_get_monitor_params` (`power.cpp`) backs its TTL off to 5 s
  when not charging. RTC-blocker resolved by analysis (see the §2.12 Deferred entry).
- **§2.18** (partial) encoder consolidation (`c7868e4` / this docs commit): new
  `hal::str_encode` module absorbs the 3× `json_escape`, 3× `b64_encode` and 2× `url_encode`
  copies (ui_chat, ui_notes_sync, ui_weather, hub). −166 net lines in the consumers. NVS
  `load/save_pref` triplet dedup + the by-design UTF-8-sanitizer decision are the leftovers.
Build/hygiene (§3): `ab01a78` `0089c55`
Dead code (§1) earlier: `e082021` `39a388a` `6449afa` `036d52a` `4db8948` `7fcda8d`
`409be67` `0e0e0f2`
Dead code (§1.2) — this session, one commit per domain:
- `0eece2c` sensors/peripherals (magnetometer, BME280, IR)
- `abe2841` core/apps (`notify::dismiss`, `secret_erase`, `home_apps_symbol`, getters)
- `f35abad` UI helpers + system-UI chain (widget factories, wifi-process-bar chain)
- `4c8a563` audio (FFT subsystem + music-list chain)
- `b010665` radio (dead TX/RX + NRF24 API)
Dead code + hygiene — this session, chronological (`<code>` / `<docs>` commit per pass):
- **§1.5/§1.7** dead types/globals + redundant decls (`6309031` / `2f65390`):
  `hw_trackball_dir`, `keyboard_type_t`+`DEVICE_KEYBOARD_TYPE`, `wifi_scan_params_t.bssid`
  (types.h + wireless.cpp memcpy), `event_define.h` NFC-type block + dead enum values,
  `NFC_TIPS_STRING` (×3 boards), `RTC_DATA_ATTR` shim, once-assigned globals
  `main_screen`/`menu_panel`/`app_panel`/`app_g`. §1.7: `notes_crypto_path_is_protected`
  dup decl dropped from `notes_crypto.h` (kept the owner in `notes_path.h`);
  `tg_get_unread_count` local re-decl → `app_registry.h` include.
- **§1.4** dead `#ifdef` branches + phantom LED slider (`ebf017e` / `641a9d1`):
  `POLLING` (both `nfc_reader.cpp` blocks → live `#else`), `ARDUINO_T_DECK_V2`
  (`display.cpp` keyboard init — kept the T_LORA_PAGER branch), `USING_TRACKBALL`
  (whole block + type aliases in `system.cpp`). `USING_LED_INDICATOR` resolved by
  REMOVING the slider (user decision): slider+cb, `hw_set_led_backlight`/
  `hw_has_indicator_led`, `led_indicator_level` + `HW_LED_INDIC_ONLINE`; bumped
  `SETTINGS_VERSION` 10→11 (one-time NVS defaults reset on upgrade).
- **§3** repo-hygiene (`73ca8e8` / `c646f11`): untracked `lib/LilyGoLib/firmware/*.bin`
  (6 × 16 MB, `git rm --cached` + `.gitignore`); pruned all 32 `src/images/*.png|jpg`
  orphans. No compiled code touched.
- **§1.6** dead LVGL v8 branches (`bc5506d` / `68dcaf0`): `ui_theme.cpp` 261→80 lines
  (v8 `#else` theme removed), `ui_define.h` v8 `#else` macro block + empty `#if v8` removed
  (v8→v9 rename shims kept). `HalResult`/`HalStatus` kept.

Each pass verified: `pio run -e tlora_pager` + `-e emulator_lora_pager` + 19/19 native tests
(§1.6 used a *clean* emulator rebuild — incremental was returning a stale "up to date").
Earlier docs/hygiene: `b8906cd` (track `OPTIMIZATION_REPORT.md`),
`880cb4c` (gitignore generated `src/*.ino.cpp`).

New HAL primitive added along the way: `hw_read_sd_stream()` (`storage.h`/`storage.cpp`)
— chunked SD read that releases the SPI bus between chunks; used by the §2.4 voice-memo
streaming. It made `hw_get_file_size()` live again (was on the report's dead list).

---

## Methodology — READ THIS BEFORE REMOVING CODE

The report is stale. For every candidate function, verify against **current** source:

```bash
# A truly-dead function has ONLY its declaration (header) + definition (.cpp).
# Count references across all compiled sources — INCLUDING the compiled vendor tree,
# which can call back into our code (see catch #3):
grep -rIn "\bFUNC\b" src/ lib/LilyGoLib/src/ --include=*.cpp --include=*.c --include=*.h --include=*.ino | wc -l
# 2 == dead (decl + def).  >2 == inspect the extra lines: a call site => LIVE.
# NOTE: matches in lib/LilyGoLib/examples/ are the vendor's STALE, non-compiled copy
# of this tree (src_dir = src) — ignore them. matches in lib/LilyGoLib/src/ are REAL.
# Then confirm the extras are only in the owning HAL .h/.cpp, never an app/ui caller:
grep -rIn "\bFUNC\b" src/ --include=*.cpp --include=*.h --include=*.ino | sed 's/:.*//' | sort | uniq -c
```

**Three real catches this discipline made — all would have shipped a regression:**

1. **`hw_get_file_size`** — on the report's §1.2 dead list, but the §2.4 voice-memo
   work made it a live caller. Kept it.
2. **`hw_set_msc_prefer_sd`** — nearly removed because an ad-hoc filter
   `grep -v "storage.cpp"` also swallowed **`settings_storage.cpp`** (substring match!),
   hiding its one live caller. When excluding the owning file, anchor the path:
   `grep -vE "src/hal/storage\.(cpp|h):"`, not `grep -v "storage.cpp"`.
3. **`ui_lock`/`ui_unlock`** (this session) — the report listed them as dead "compat
   aliases," and they have **zero callers in `src/`**. But the **compiled vendor**
   `lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp` declares them `extern` and calls them
   (~lines 1309–1319) from the SPI-guard path. Removing them = link error. **This is why
   the grep above must include `lib/LilyGoLib/src/`.** A comment in `ui_main.cpp` even
   said "Called from the vendor LilyGoLib paths" — read the code around a candidate, not
   just the grep count.

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

## §1.2 sweep — completed record (by risk tier)

**All groups below are done.** Kept as a record of what was removed, the cascades
followed, and (crucially) the deliberate *conservative* choices in the HIGH-risk tier.

### LOW risk — the routine sweep
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

### HIGH risk — done as dedicated passes (⚠️ compile+emulator only, see smoke-test checklist)
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
- ~~**radio**~~ ✅ done (compile + emulator verified; **not** hardware-smoke-tested).
  Removed, from the **compiled** files only, everything with zero call sites:
  - `radio_common.cpp`: `hw_set_radio_listening`/`hw_set_radio_tx`/`hw_get_radio_rx`/
    `radio_transmit` + the now-unused `last_send_millis`.
  - `radio.cpp`: `hw_set_usb_rf_switch`.
  - `radio/nrf2401.cpp`: the 7 dead NRF24 API functions (`hw_has_nrf24`,
    `hw_get/set_nrf24_params`, `hw_set_nrf24_listening`, `hw_clear_nrf24_flag`,
    `hw_set_nrf24_tx`, `hw_get_nrf24_rx`).
  - `radio/sx1262.cpp`: the option-table arrays + the `radio_get_*_from_index`/`_length`
    trio (also clears the `RADIO_FIXED_FREQUENCY` §1.4 branch).
  - `radio.h`: all the above decls.

  **Conservative choices (deliberate — this is boot/ISR code and was NOT hardware-tested):**
  1. **Kept `hw_radio_begin` and `hw_nrf24_begin` (+ their `hw_radio_isr`/`hw_nrf24_isr`
     and `radioEvent`) fully intact.** They're called at boot. After removing the dead
     TX/RX consumers the ISR/event-group is now *inert* (nothing waits on the events), but
     emptying a live boot function / dropping `setPacketSentAction` is exactly the kind of
     change that can brick boot without a hardware smoke-test. The report says this
     plumbing "shrinks to nothing" — that final trim is left for a **hardware-validated**
     pass. `hw_nrf24_begin` + `USING_EXTERN_NRF2401` likewise kept (live boot init).
  2. **Did NOT touch the non-compiled chip files** (`cc1101.cpp`, `lr1121.cpp`,
     `sx1280.cpp` — SX1262 is the selected chip). Their `radio_get_*` trios remain as
     harmless dead source (0 flash, not compiled). Removing the shared `radio.h` decls is
     safe for them (their defs are self-declaring). `lr1121.cpp` additionally writes
     `_high_freq` from its **live** `configure()` but only reads it in the trio — trimming
     that needs an LR1121 build to verify. Do these when/if that chip is built.
  The radio is configured-but-idle in this firmware (like GPS): `hw_set_radio_enable` sets
  frequency/params at boot but nothing ever TX/RXes — which is why the whole TX/RX/NRF24
  API was dead.

---

## Hardware smoke-test checklist (do before trusting the radio + audio-FFT passes)

Commits `b010665` (radio) and `4c8a563` (audio) were verified by `pio run -e tlora_pager`
+ `pio run -e emulator_lora_pager` + `pio test -e native_test` only. They touch (or sit
next to) boot/ISR/DMA paths that a compile cannot exercise. On a physical T-LoRa-Pager,
flash `pio run -e tlora_pager -t upload` and confirm:

- [ ] **Boots to the home screen** (no boot loop / panic on serial `@115200`). This alone
      clears the biggest radio worry — `hw_radio_begin`/`hw_nrf24_begin` still run at boot.
- [ ] **Radio settings work** — Settings » Connectivity » Radio toggle flips without hang
      (exercises the kept `hw_set_radio_enable` → `hw_set_radio_default` → chip `configure`).
- [ ] **Audio still plays** — play an MP3/WAV (exercises the kept player task + codec path
      that sits right below the removed FFT block).
- [ ] **Voice recording still works** — record + play back an audio note (the `hw_rec_*`
      path shares the codec with the removed mic/FFT code; confirm codec open/close is fine).
- [ ] No new serial warnings/errors around radio or codec init.

If all pass, delete this checklist and the "compile-only" caveats above. If the radio
boot is solid, the *optional* follow-up trims become safe: empty `hw_radio_begin`/
`hw_nrf24_begin`'s now-inert ISR/event-group setup, and remove the dead `radio_get_*`
trios from the non-compiled chip files (`cc1101`/`lr1121`/`sx1280`, minding `lr1121`'s
`_high_freq`) after building each with its chip macro.

---

## Deferred / judgement-call items

- **§2.16 (`hw_http_request` double-buffer)** — deferred. The safe fix (stream
  `getStream()` into `out`) means hand-rolling a bounded read loop over the embedded TLS
  stream with timeout + chunked-encoding handling; a mistake truncates responses or hangs
  a fetch. The benefit is one transient copy of a small (~2–5 KB) body that frees
  immediately. Poor risk/reward vs. the robust `getString()`. Revisit only if profiling
  shows internal-heap pressure during TLS.
- ~~**§2.12 (status-bar RTC + gauge sweep)**~~ ✅ **DONE.** Blocker ("confirm the RTC is not
  the post-deep-sleep source of truth") resolved by reading the time/wake flow:
  `hw_init` (`system.cpp:207`) seeds the ESP32 system clock from the external RTC via
  `settimeofday()` at every boot, and NTP (`configTime` + SNTP callback), GPS
  (`gps_time_sync.cpp`), and manual sets (`hw_set_date_time`) all write **both** the system
  clock and the RTC. Crucially there is **no `esp_deep_sleep`/reboot path in `src/`** — the
  firmware uses *fake sleep* (`hw_power_down_all` lowers CPU freq) / *light sleep*
  (`hw_light_sleep` → `instance.lightSleep`), both of which keep the system clock running.
  So `time(nullptr)`+`localtime_r` is a valid source of truth for the display clock. Fix
  applied: new `hw_get_wall_clock()` (no I2C, no instance-mutex hold) drives the two 1 Hz
  ticks; the gauge sweep backs off to a 5 s TTL when not charging. `hw_get_date_time()`
  (RTC-authoritative) is unchanged and still used by settings/info/one-off timestamps.
- **§1.5 `monitor_params_t` write-only fields** — the 14 gauge/voltage fields
  (`type`, `charge_state`, `sys_voltage`, `battery_voltage`, `usb_voltage`,
  `remainingCapacity`, `fullChargeCapacity`, `designCapacity`, `instantaneousCurrent`,
  `standbyCurrent`, `averagePower`, `maxLoadCurrent`, `timeToEmpty`, `timeToFull`,
  `ntc_state`) plus `monitor_params_type_t` are write-only (only `battery_percent`,
  `is_charging`, and internally `battery_voltage` are read). **Left for the §2.12 gauge
  sweep, not this dead-type pass:** each dead field is populated by a separate
  `instance.gauge.*`/`instance.ppm.*`/`instance.pmu.*` register read inside
  `hw_get_monitor_params` (`power.cpp`), executed every second (now every 5 s when idle
  after §2.12). Removing the fields still removes live I2C traffic — a hardware-behaviour
  change that wants a device to validate, so it stays deferred to the hardware pass (§2.12
  merely throttled the sweep cadence; it did not trim any field).
  (`getFaultStatus()` is called for its clear-fault side effect and must be kept.)
- **§1.7 factory.ino timezone externs** — `timezone_get_user_tz`/`timezone_fetch_offset`
  (declared in the *private* `settings_internal.h`) and `timezone_get_user_posix` (no
  header at all) are ad-hoc `extern`-declared in `factory.ino` and re-declared again in
  `hal/gps_time_sync.cpp`. They are **functional, not dead** — the fix is consolidation
  into a shared header, but that's blocked by a layering inversion: the functions are
  *defined* in `ui_time_sync.cpp` (UI layer) yet *consumed* by `hal/gps_time_sync.cpp`
  (HAL). A UI header included from HAL would formalise a HAL→UI dependency the layering
  forbids; the clean fix is relocating the timezone logic to a neutral/HAL module first.
  Deferred until that relocation is in scope.
- **§1.6 `HalResult`/`HalStatus`** — KEPT (not removed). Referenced only by
  `test/test_hal_result`, but CLAUDE.md calls it the *preferred* error type for new code.
  Keep unless that migration is abandoned. (The rest of §1.6 — the dead LVGL v8 theme
  branch — is done; see the §1.6 row/commit note.)
- **Scattered `#if LVGL_VERSION_MAJOR == 9` guards** (`ui_tools.cpp` ×6, `ui_settings.cpp`,
  `ui_journal.cpp`) — NOT touched. These were outside the report's §1.6 scope (which named
  only `ui_theme.cpp`/`ui_define.h`). They are small version guards, not a big dead branch;
  unwrapping them is low-value churn. Fold into a future pass only if simplifying all v9
  guards at once. Generated font `.c` files' v8 blocks stay (LVGL-converter output).
- **§1.4 `USING_LED_INDICATOR`** — ✅ RESOLVED: the phantom LED slider and all its plumbing
  were removed (the pager has no user-facing indicator LED to drive). See the §1.4 row and
  the §1.4 commit note above.
- **§1.4 `USING_SI473X_RADIO` / `USING_QMI8658_SENSOR`** — left in place *by design*, not
  overlooked. Both live only as slots in the positional `hw_devices[]` name table
  (`system.cpp`), each `#ifdef NAME "name" #else "" #endif`. The `#else ""` is intentional
  structure: the slot index maps to a device-online bit, and the name documents which
  device that slot is for. Collapsing the two dead entries to a bare `""` yields identical
  compiled output (0 flash) while deleting that documentation and risking index drift —
  strictly worse. Removing the *device concept* (slot + online bit + renumber) is a bigger,
  riskier change out of scope here. `ARDUINO_T_DECK_V2` in `lr1121.cpp` is likewise left
  under the non-compiled-chip-file discipline (see radio note).

---

## Repo-hygiene leftovers (safe, not yet done)

- ~~`lib/LilyGoLib/firmware/*.bin` (~96 MB tracked)~~ ✅ untracked (§3.5). Still in git
  *history* — clone size only shrinks after a history rewrite (out of scope, disruptive).
- ~~`src/images/` PNG/JPG orphans~~ ✅ all 32 removed (§3.2).
- `test/test_desktop/test_main.cpp` — 26-line `2+2` placeholder (§3.5).
- Duplicated helpers to consolidate (§2.18): ~~`url_encode` twice within `ui_weather.cpp`;
  `json_escape`/`b64_encode` duplicated across `ui_chat.cpp`, `ui_notes_sync.cpp` and
  `hub.cpp`~~ ✅ done — hoisted into `hal::str_encode` (`c7868e4`). **Still open**: the NVS
  `load/save_pref` triplets re-implemented across ~5 files (`ui_journal`, `ui_notes_sync`,
  `ui_tasks`, `ui_telegram`, `ui_text_editor`; canonical pair likely belongs in
  `storage.{h,cpp}`). Persistence-touching — do it as its own careful pass, ideally with a
  device to confirm no NVS-namespace/key regressions. The weather/telegram UTF-8 sanitizers
  are **left by design** (shared decode skeleton, genuinely different downstream logic).

---

## Suggested next order

**The whole §1 dead-code audit (§1.1–§1.7) and §3 repo-hygiene are DONE** (the §1.4/§1.5/§1.6
leftovers are deliberate, documented in Deferred). What remains, best-first:

1. **Remaining §2 perf** — `§2.13` (audio busy-wait, needs care), `§2.17` (settings
   blocking HTTPS, user-initiated), `§2.18` NVS `load/save_pref` triplet dedup (the encoder
   half of §2.18 is done — see the table row; the sanitizer half is left by design). `§3.1`
   font-picker cap is a product decision still open. (`§2.12` is done; the write-only
   `monitor_params_t` §1.5 fields are still deferred to the hardware pass, decoupled from
   §2.12.)
2. **§4 Go server** (`server/`) — independent codebase; A1/A3/B4/B5/B6 are the concrete items.
3. **When hardware is available** — run the [smoke-test checklist](#hardware-smoke-test-checklist-do-before-trusting-the-radio--audio-fft-passes)
   to validate the radio/audio passes, then optionally do the follow-up trims it lists.
