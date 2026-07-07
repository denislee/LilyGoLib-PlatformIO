# Optimization Progress & Handoff

Companion to `OPTIMIZATION_REPORT.md` (the analysis). This file tracks **what has
been applied**, **what remains**, and — most importantly — **how to verify safely**
before removing anything else. Last updated **2026-07-07**.

> **Milestone: the entire §1.2 dead-function sweep (~60 never-called `hw_*`
> functions, all domains) is COMPLETE.** §1.1/§1.3 are also done. What's left is
> §1.4/§1.5/§1.6/§1.7 (dead types/globals/ifdefs/decls), the remaining §2 perf items,
> §3 hygiene (untrack `firmware/*.bin`, prune `src/images/`), and the §4 Go server.
> See [Suggested next order](#suggested-next-order).
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
| 1.4 | Dead `#ifdef` branches | ◐ partial — `USING_UART_BLE`, `USING_MAG_QMC5883`, `USING_BME280`, `USING_IR_REMOTE`/`_RECEIVER`, `RADIO_FIXED_FREQUENCY` gone (cleared as side effects of §1.2); **rest remain** (`ARDUINO_T_DECK_V2`, `POLLING`, `USING_TRACKBALL`, `USING_SI473X_RADIO`, `USING_QMI8658_SENSOR`, `USING_LED_INDICATOR` bug candidate, …) |
| 1.5 | Dead types/fields/globals | ◐ partial — FFT types (`FFTData`/`FFT_SIZE`/`SAMPLE_RATE`/`FREQ_BANDS`) removed with the audio pass; **rest remain** (`hw_trackball_dir`, `keyboard_type_t`, write-only `monitor_params_t` fields, `event_define.h` NFC block, once-assigned `main_screen`/`menu_panel`/`app_panel`/`app_g` globals, …) |
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
- Current image: RAM 27.4 % (89,656 B) · Flash 70.5 % (2,955,369 B) — **unchanged by
  the §1.2 sweep**: everything removed was already `--gc-sections`-stripped, so those
  deletions are build-time + source-hygiene wins, not flash/RAM.
- **Source reduction from the §1.2 completion this session: ~1,200 lines** across
  sensors/peripherals, core/apps, UI helpers, audio, and radio (see commits below).

The big *perceptual* wins are the §2.1/§2.4/§2.6 telegram+chat changes: the UI no
longer freezes ~1–2 s per poll or per voice-memo send.

---

## Completed commits (this effort)

Perf (§2): `3b373da` `46686f3` `932eed0` `ba2a4ae` `c22a83e` `e897fe1` `37d67e5`
`bc78d41` `c7ff2c2` `5c4ab5f` `590faf4` `9c60c5b` `c01a540` `f6d90ed`
Build/hygiene (§3): `ab01a78` `0089c55`
Dead code (§1) earlier: `e082021` `39a388a` `6449afa` `036d52a` `4db8948` `7fcda8d`
`409be67` `0e0e0f2`
Dead code (§1.2) — this session, one commit per domain:
- `0eece2c` sensors/peripherals (magnetometer, BME280, IR)
- `abe2841` core/apps (`notify::dismiss`, `secret_erase`, `home_apps_symbol`, getters)
- `f35abad` UI helpers + system-UI chain (widget factories, wifi-process-bar chain)
- `4c8a563` audio (FFT subsystem + music-list chain)
- `b010665` radio (dead TX/RX + NRF24 API)
Docs/hygiene this session: `b8906cd` (track `OPTIMIZATION_REPORT.md`),
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

**§1.1–§1.3 and the whole §1.2 sweep (incl. audio + radio) are DONE.** What remains,
best-first:

1. **§1.5 / §1.7** — dead types/fields/globals and redundant declarations. Best next
   coding step: many now pair with functions already removed (e.g. `hw_trackball_dir`,
   `keyboard_type_t`, the once-assigned `main_screen`/`menu_panel`/`app_panel`/`app_g`
   globals, the write-only `monitor_params_t` fields, the `event_define.h` NFC block,
   duplicate `isinMenu`/`notes_crypto_path_is_protected` decls). Same verified sweep;
   watch for §1.5's tie-in with perf §2.12 (the write-only gauge fields).
2. **§1.4 remaining dead `#ifdef` branches** — mostly mechanical, but **`USING_LED_INDICATOR`
   is a functional-bug decision, not just dead code** (see Deferred). Make that call first.
3. **Repo-hygiene (§3.5/§3.2)** — untrack `lib/LilyGoLib/firmware/*.bin` (~96 MB) and
   prune the 25 orphan PNG/JPG sources in `src/images/`. Zero code risk; big repo-size win.
4. **§1.6** — judgement calls (LVGL v8 theme `#else` branch is safe to drop since every
   env pins v9; keep `HalResult`/`HalStatus` unless the migration is abandoned).
5. **Remaining §2 perf** — `§2.12` (status-bar RTC/gauge, needs the wake/time-seed flow
   understood — see Deferred), `§2.13` (audio busy-wait), `§2.17`, `§2.18` (helper dedup).
6. **§4 Go server** (`server/`) — independent codebase; A1/A3/B4/B5/B6 are the concrete items.
7. **When hardware is available** — run the [smoke-test checklist](#hardware-smoke-test-checklist-do-before-trusting-the-radio--audio-fft-passes)
   to validate the radio/audio passes, then optionally do the follow-up trims it lists.
