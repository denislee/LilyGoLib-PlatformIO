# Optimization — Battery / Power Analysis & Handoff

Generated **2026-07-21** on the post-phase-3 tree (working tree at `4ed512f` + the
uncommitted P3.9/P3.14 watermark instrumentation and the unrelated settings-shortcut
feature diff). Produced by a four-way sweep (sleep/power-rails, wireless, periodic
tasks/timers, display/sensors); **every headline claim below was re-verified against
current source** before being recorded (the standing lesson from phases 1–3: reports
go stale and agents mis-read — re-verify again before *editing*).

**Relationship to the other docs:**
- `OPTIMIZATION_PROGRESS.md` — phase-1 tracker; its **Methodology** and the
  **hardware smoke-test checklist** apply verbatim here. Several PB items below add
  to that checklist.
- `OPTIMIZATION_PHASE2.md` / `OPTIMIZATION_PHASE3.md` — phases 2–3. The battery items
  they already track are cross-referenced, not duplicated: **P2.12** (WiFi power-save)
  is promoted to PB.2, **P3.12** (SD rail) to PB.5, **§1.5** (write-only gauge fields)
  to PB.14 — those stay the canonical IDs; this file adds the battery framing and the
  new neighbours they compound with.
- This file — the battery-focused handoff, numbered `PB.x`.

**⚠️ Measurement discipline (this doc's own P3.7 rule):** every mA figure below is a
**datasheet/estimate**, not a bench reading. None of this codebase's power behaviour
has ever been current-measured. Before investing in the medium/high-effort items,
put the device on a USB power meter / PPK (battery path if possible) and record the
baseline table in §Bench appendix. Do not "fix" an item whose measured cost turns out
to be noise, and do not trust the estimates over the meter.

**Build discipline (unchanged):** after every change,
`pio run -e tlora_pager && pio run -e emulator_lora_pager && pio test -e native_test`.
Anything touching task loops / ISRs / rails / sleep paths is ⚠️
**hardware-test-required** — add it to the smoke-test checklist in
`OPTIMIZATION_PROGRESS.md`.

---

## 1. Current power model (verified 2026-07-21)

The firmware has **four** power states. Only the first two are reachable without a
button press; there is **no automatic transition into any of them** (see PB.1).

### 1.1 Active (screen on, recent input)
- CPU at `user_setting.cpu_freq_mhz` (default 240 MHz).
- LVGL task wakes at **~60 Hz even with static content** — `kMaxTickMs = 16`
  (`hal/lvgl_task.cpp:41,67`) caps the sleep below LVGL's own 33 ms refresh period
  (PB.8). Each wake takes the instance mutex.
- Keyboard task polls the TCA8418 over I2C at **100 Hz** (`hal/keyboard_task.cpp`,
  `kPollMs = 10`) (PB.9). Vendor rotary task polls GPIO at 500 Hz (cheap, vendor code,
  informational only — phase-3 note stands).
- `loopTask` at 20 Hz (`factory.ino` `delay(50)`); `instance.loop()` is an EventGroup
  read, no I2C on idle ticks.
- Status-bar 1 Hz lv_timer: wall-clock from sysclock (no I2C, §2.12 done), gauge sweep
  behind a 5 s TTL when discharging, FFat file count every 60 ticks, and a
  **hub TCP probe every 10 ticks** when the hub is enabled (PB.10).
- Telegram background poll: 60 s lv_timer, spawns an HTTPS fetch when its guards pass
  (PB.11).
- Backlight: AW9364, 17 levels; default `brightness_level = 50` is clamped to max
  (16) — i.e. **default is full brightness**, and there is no auto-dim of any kind.

### 1.2 Idle-awake (no input > 2 s, screen still on)
- CPU drops to 80 MHz (`factory.ino` inactivity check). **Nothing else changes** —
  full brightness, 60 Hz LVGL, 100 Hz keyboard, all timers.
- `user_setting.disp_timeout_second` (Settings → Display, slider 0–180 s) is
  **written and persisted but never read by any sleep/dim logic** — the timeout knob
  is dead (PB.1). A device left face-up never sleeps.

### 1.3 Fake sleep (rotary center-button ≥ 1 s → vendor `perform_fake_sleep_toggle()` → `ui_pause_timers()` → `hw_power_down_all()`)
Off / gated:
- Panel: `sleepDisplay()` sends ST7796 SLPIN; backlight 0. ✅
- Rails cut (`hal/system.cpp:662-679`): keyboard (+`kb.end()`), NFC, haptic, speaker. ✅
- GPS: permanently off since boot (P2.5 ✅).
- CPU: 40 MHz — **or 80 MHz if WiFi or BLE is connected** (`hold_80`,
  `system.cpp:675-677`) (PB.20).
- LVGL / keyboard / rotary / NFC tasks: notify-block on `ulTaskNotifyTake(…, 200 ms)`
  (P2.1/P2.2/P3.13 ✅). All lv_timers are de-facto frozen because `lv_timer_handler()`
  is never called. Note the 200 ms timeouts mean each of these four tasks still
  wakes 5×/s as a fallback (PB.16).

Still running / still powered — **this is the fake-sleep gap list**:
- **SX1262 in STANDBY** (~0.6–1.6 mA). `radio.sleep()` is never called anywhere in
  `src/`; even the settings "radio off" maps `RADIO_DISABLE` → `radio.standby()`
  (`hal/radio/sx1262.cpp:68`) (PB.3).
- **BQ25896 continuous ADC measurement stays enabled** (~0.5–1 mA); the vendor's own
  `lightSleep()` calls `ppm.disableMeasure()` — our fake sleep doesn't (PB.4).
- **SD rail on** (~0.5–1 mA), deliberate skip with comment (P3.12 / PB.5).
- **BHI260 keeps running 3 virtual sensors** (accel 25 Hz, rotation 25 Hz,
  orientation 5 Hz), filling a FIFO nobody drains during sleep (~0.5–1 mA) — and the
  feature they feed (`glance_show()`) has **zero call sites** (PB.6).
- **WiFi fully associated** at the Arduino default power-save (`WIFI_PS_MIN_MODEM`;
  no explicit `esp_wifi_set_ps`/`WiFi.setSleep` call exists) (PB.2), and it forces
  the 80 MHz CPU floor (PB.20).
- **BLE**: if the BLE keyboard is enabled, connection params force a wake per
  connection event (slave latency 0, 30–50 ms interval), advertising runs forever
  when unconnected, and the 1 Hz `ble_kb_ka` task is not fake-sleep gated (PB.12).
- Charge task polls `ppm.isVbusIn()` over I2C at 2 Hz (its awake state blocks until
  sleep entry — this task only costs during fake sleep) (PB.13). `loopTask` at 2 Hz;
  vendor rotary at 10 Hz (GPIO-only, needed for the wake long-press).

### 1.4 True light sleep / deep sleep (PMU button long-press → `hw_low_power_loop()`)
- `sleep_mode == 0`: vendor `lightSleep()` → `esp_light_sleep_start()`. The vendor
  path cuts **everything** our fake sleep leaves on: `ppm.disableMeasure()`,
  `radio.sleep()`, SD unmount + rail cut, all expander rails
  (`lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp:678-764`). Wake source: boot button only.
- `sleep_mode == 1`: `hw_sleep()` → deep sleep, notes passphrase locked first.
- **No ESP-IDF automatic light sleep / tickless idle / `esp_pm_configure` anywhere**
  (PB.21, investigate-only).

The one-line summary: **fake sleep is the state the device actually lives in, and it
was tuned for CPU/lock churn (phases 2–3), not for rail/chip power. The vendor's
`lightSleep()` is the reference for what fake sleep still leaves on.**

---

## 2. Already-tracked battery items — status (do not redo)

| ID | Item | Status |
|---|---|---|
| P2.1 | Keyboard task fake-sleep notify-block | ✅ done (HW smoke-test still owed) |
| P2.2 | NFC task gate + rail cut when disabled/sleeping | ✅ done (HW smoke-test still owed) |
| P2.5 | GPS rail transient-only, toggle removed | ✅ done |
| P3.13 | Rotary task fake-sleep notify-block | ✅ done (HW smoke-test still owed) |
| §2.12 | Status-bar sysclock + gauge 5 s TTL | ✅ done |
| P3.16 | Double `setCpuFrequencyMhz` at boot | ✅ done (but see PB.17 — the *transition* double-set is new and different) |
| P2.12 | WiFi power-save | ☐ open → **PB.2** |
| P3.12 | SD rail through fake sleep | ☐ open → **PB.5** |
| §1.5 | Write-only `monitor_params_t` gauge fields | ☐ open → **PB.14** (with a correction: `.temperature` is *also* write-only — 9 removable register reads, not 8) |

---

## 3. Executive summary — new opportunities

Estimates assume the ~1000 mAh battery the P3.12 analysis cites. Fake-sleep
parasitics stack: closing PB.2–PB.6 is worth an estimated **3–8 mA continuous during
sleep** (more if WiFi stays connected), i.e. potentially days of standby.

| # | Finding | Est. impact | Risk / effort |
|---|---|---|---|
| PB.1 | `disp_timeout_second` is a dead knob — **no automatic sleep at all** | Largest real-world lever (display + everything stays on indefinitely) | Med effort, Low risk / ⚠️ HW |
| PB.2 | WiFi PS never configured; escalate to `WIFI_PS_MAX_MODEM` in fake sleep (= P2.12) | ~1–5 mA in sleep w/ WiFi | Low / product latency call |
| PB.3 | SX1262 never sleeps — STANDBY forever, even "disabled" | ~0.6–1.6 mA, 24/7 | Low code / ⚠️ HW (boot/ISR adjacency) |
| PB.4 | BQ25896 ADC continuous through fake sleep (vendor sleeps it) | ~0.5–1 mA in sleep | Very low / ⚠️ HW trivial |
| PB.5 | SD rail through fake sleep (= P3.12) | ~0.5–1 mA in sleep | Med (remount) / ⚠️ HW |
| PB.6 | BHI260 3 virtual sensors 24/7 feeding a never-called feature; `hw_unregister_imu_process()` only disables 1 of 3 | ~0.5–1 mA, 24/7 | Low (option B) / product call (option A) |
| PB.7 | NFC runs full RF-field polling; ST25R3916 Wake-Up mode (~75 µA) unused | 5–15 mA whenever NFC enabled | Med / ⚠️ HW |
| PB.8 | LVGL task wakes 60 Hz on static content (`kMaxTickMs=16`) | CPU churn + mutex contention, awake | Low |
| PB.9 | Keyboard 100 Hz I2C poll awake (`kPollMs` 10 → 20–30) | I2C/lock churn, awake | Low / ⚠️ HW feel-check |
| PB.10 ✅ | Hub TCP probe every 10 s (cache TTL is 30 s) | 1 TCP+task spawn/10 s | Trivial — **done 2026-07-21** |
| PB.11 | Telegram bg poll every 60 s (TLS round-trip) | WiFi airtime each minute | Product call |
| PB.12 | BLE: slave latency 0; advertising forever; `ble_kb_ka` 1 Hz un-gated; TX power default | ~1–2 mA connected; ~0.1–0.3 mA advertising | Low–Med / ⚠️ HW |
| PB.13 | Charge task 2 Hz I2C VBUS poll in sleep → PMU VBUS-insert IRQ | small I2C churn in sleep | Med (IRQ plumbing) / ⚠️ HW |
| PB.14 | Gauge sweep reads 9 write-only registers per sweep (= §1.5, +`.temperature` correction) | I2C churn each sweep | Low / ⚠️ HW pass |
| PB.15 ✅ | `hw_get_battery_voltage()` bypasses the TTL cache — full `gauge.refresh()` at 1 Hz on the info page | I2C churn while page open | Trivial — **done 2026-07-21** |
| PB.16 | Fake-sleep notify-blocks use 200 ms timeouts → 4 tasks × 5 wakes/s | scheduler churn in sleep | Low |
| PB.17 | CPU freq set twice on every fake-sleep entry/exit (`hw_power_*_all` + `loop()` both own it) | cosmetic (~0.5 ms PLL) | Trivial |
| PB.18 ✅ | `hw_light_sleep()` dead code; would skip `notes_crypto_lock()` if ever called | hygiene + latent security gap | Trivial — **done 2026-07-21** |
| PB.19 | `speaker_enable=1` latches the amp rail on between sessions (default off; codec PA-callback self-gates playback) | 2–8 mA only if user enables toggle | Low |
| PB.20 | 80 MHz fake-sleep CPU floor whenever WiFi/BLE connected | ~20–30 mW in sleep | Med / ⚠️ HW, compound with PB.2 |
| PB.21 | No ESP-IDF auto light sleep / tickless idle; no periodic true-light-sleep during fake sleep | potentially the deepest saver | Investigate-only / high |

---

## 4. Detailed findings

### A. Structural — make sleep happen, then close its gaps

#### PB.1 — `disp_timeout_second` is a dead setting: the device never sleeps on its own (HIGHEST VALUE)

**Where:** `apps/settings_display.cpp:57-68,235-241` (slider writes + persists it),
`hal/types.h:184` (field), `hal/system.cpp:343,418` (default 0), and — the gap —
`factory.ino` reads `lv_display_get_inactive_time(NULL)` only to drop the CPU to
80 MHz at > 2 s; **nothing anywhere compares the inactivity clock to
`disp_timeout_second`**. Grep-verified: the only reads of the field are the settings
UI's own display of it.

**Cost today:** every mA in this document is conditional on the user remembering the
rotary long-press. A pager left on a desk runs full backlight (default = max
brightness — the default `brightness_level = 50` clamps to the AW9364 max of 16),
60 Hz LVGL, 100 Hz keyboard I2C, radio standby, IMU, forever. This dwarfs every
other item.

**Fix direction:** in `factory.ino::loop()` where `inactive_time` is already read:
when `!ui_is_fake_sleep() && disp_timeout_second > 0 && inactive_time >= timeout`,
enter fake sleep. Two plumbing notes:
1. The entry point is the vendor's `perform_fake_sleep_toggle()`
   (`LilyGo_LoRa_Pager.cpp:1301`), driven from its rotary task. Phase-1 removed the
   `lilygo_request_fake_sleep_toggle` extern as dead — **re-introduce that hook** (or
   mirror the toggle body: `setBrightness(0)` + `sleepDisplay()` + `ui_pause_timers()`)
   rather than duplicating the sequence ad-hoc. Keep entry/exit symmetric with the
   long-press path so wake behaves identically.
2. Guards: don't fire while audio is playing/recording (`playerEvent` bits /
   recorder running), while an SSH session is connected, or while text input is
   focused (`core::isTextInputFocused()` — same guard the telegram poll uses).
   Encoder/keyboard activity already resets the LVGL inactivity clock via the input
   drivers, so wake-then-idle re-arms correctly for free.

**Optional stage 2 (separate commit):** two-stage dim — at `timeout − 5 s` drop
brightness to ~2 as a warning (vendor `decrementBrightness()` exists; `hw_shutdown()`
already uses it), restore on interaction. UX polish, small extra state.

**Risk:** Low-Med. The mechanism is proven (it's the long-press path); the new part
is only the trigger + guards. ⚠️ HW: verify auto-entry never fires mid-playback /
mid-sync, and that wake latency matches the manual path (P2.1/P3.13 checklist items
already cover first-input-after-wake).

#### PB.2 — WiFi power-save (promotes P2.12): explicit MIN_MODEM + escalate to MAX_MODEM during fake sleep

**Where:** `hal/wireless.cpp:125-144` (`hw_set_wifi_enable` — `WiFi.mode(WIFI_STA)`
with no power-save call anywhere after it; grep-verified zero
`WiFi.setSleep`/`esp_wifi_set_ps` in `src/`). Fake-sleep entry/exit:
`hal/system.cpp` `hw_power_down_all()`/`hw_power_up_all()`.

**Cost today:** the stack runs at the Arduino-ESP32 *default* (`WIFI_PS_MIN_MODEM`
per the phase-2 analysis — worth confirming once on serial, since nothing in this
tree ever sets it). MIN keeps the RF front-end waking every DTIM beacon; MAX lets it
sleep for the listen interval, saving an estimated 1–5 mA while associated — exactly
the state fake sleep spends hours in. Auto-reconnect is already off
(`factory.ino:108`), so there is no reconnect churn to worry about.

**Fix direction:**
- On enable: `WiFi.setSleep(WIFI_PS_MIN_MODEM)` explicitly (determinism, not a
  behaviour change).
- In `hw_power_down_all()`: `esp_wifi_set_ps(WIFI_PS_MAX_MODEM)` when connected;
  restore MIN in `hw_power_up_all()`.

**Trade-off (the product decision phase 2 parked):** MAX_MODEM adds up to a few
hundred ms latency to *inbound* packets during sleep. The only background consumer
is the 60 s telegram poll — outbound-initiated, entirely tolerant. If in doubt, gate
behind a settings toggle ("Deeper WiFi sleep").

**Risk:** Low. ⚠️ HW: confirm telegram bg poll still completes during fake sleep
(it doesn't — lv_timers freeze — so really: confirm reconnect/first-fetch-after-wake
is unaffected) and that the hub probe TTL behaviour is unchanged after wake.

#### PB.3 — SX1262 never leaves STANDBY: sleep it when disabled and on fake-sleep entry

**Where:** `hal/radio/sx1262.cpp:68` — `case RADIO_DISABLE: state = radio.standby();`
(and `hal/radio.cpp:22-26`: the settings "radio off" path ends there). No
`radio.sleep()` call exists in `src/` (grep-verified). `hw_power_down_all()` doesn't
touch the radio. Vendor `lightSleep()` *does* call `radio.sleep()`
(`LilyGo_LoRa_Pager.cpp:687`).

**Cost today:** SX1262 STANDBY_RC ≈ 0.6–1.6 mA vs < 1 µA in sleep — continuously,
in every state, regardless of the radio toggle. On a ~1000 mAh battery that is
~15–40 mAh/day for a chip this firmware never TX/RXes with (phase-1 established the
radio is configured-but-idle).

**Fix direction (two independent halves):**
1. `RADIO_DISABLE` → `radio.sleep()` in `configure()`. RadioLib requires a wake
   (standby/begin) before the next register write after sleep — put the wake at the
   top of `configure()` (or in `hw_set_radio_default()`), so every reconfigure path
   is self-healing.
2. `hw_power_down_all()`: `radio.sleep()` under `core::ScopedSpiLock`;
   `hw_power_up_all()`: re-apply `hw_set_radio_enable(user_setting.radio_enable)`.

**Risk:** Low in code, but ⚠️ **HW-test-required with extra caution**: this sits next
to the phase-1 radio dead-code passes that are *themselves* still awaiting the
hardware smoke-test (boot ISRs kept conservatively). Land PB.3 only after (or
together with) that checklist run: boot, toggle radio in settings, sleep/wake cycle,
no SPI errors on serial.

#### PB.4 — BQ25896: disable continuous ADC during fake sleep (vendor parity, near-free)

**Where:** `hw_power_down_all()` (`hal/system.cpp:662-679`) — no
`instance.ppm.disableMeasure()`. Vendor `lightSleep()` calls it on entry and
`enableMeasure()` on wake (`LilyGo_LoRa_Pager.cpp:685,756`).

**Cost today:** continuous ADC conversion ≈ 0.5–1 mA for data nobody consumes during
sleep — the LVGL task is blocked, so no gauge sweep runs; the charge task reads only
`isVbusIn()` (a status register, not ADC-dependent — verify this claim on hardware
once: plug-in detection while asleep is the one behaviour that must survive).

**Fix direction:** `disableMeasure()` in `hw_power_down_all()`, `enableMeasure()` in
`hw_power_up_all()`, mirroring the vendor. ⚠️ HW: charger-plug during fake sleep
still wakes the display (the charge task's VBUS edge), and charging state renders
correctly after wake.

#### PB.5 — SD rail through fake sleep (= P3.12, unchanged, restated for completeness)

**Where:** `hal/system.cpp:668` comment "SD Card is left on to avoid mount/unmount
overhead"; vendor `lightSleep()` does `uninstallSD()` + rail cut
(`LilyGo_LoRa_Pager.cpp:701-703`).

Still open, still the same shape: ~0.5–1 mA for hours of sleep, fix needs a quiesce
gate (no bulk/prune/sync in flight) + reliable re-`SD.begin()` on wake.
**Bench-measure the actual mA before paying the remount-complexity cost** — this is
the flagship example of an item the meter might demote. Tracked as P3.12; keep that ID.

#### PB.6 — BHI260 runs three virtual sensors 24/7 for a feature with zero call sites

**Where:** `apps/app_registry.cpp:32` registers the IMU pipeline at boot
(comment says it drives the glance overlay); `hal/sensors.cpp:175-201` configures
ACCEL_PASSTHROUGH @ 25 Hz, GAME_ROTATION_VECTOR @ 25 Hz, DEVICE_ORIENTATION @ 5 Hz.
**`apps::menu::glance_show()` (`menu_glance.cpp:174`) has zero call sites** — the
only reference is a comment in `menu_app.cpp:316`. The sole live consumer of IMU
data is the settings IMU-debug subpage (page-gated). During fake sleep the chip
keeps computing into a FIFO nobody drains (`sensor.update()` runs from the blocked
LVGL-task path).

**Prerequisite bug:** `hw_unregister_imu_process()` (`hal/sensors.cpp:219-232`)
disables **only** GAME_ROTATION_VECTOR — ACCEL and DEVICE_ORIENTATION keep running
after "unregister". Fix first (two more `configure(…, 0, 0)` calls), whatever else
is decided.

**Cost today:** ~0.5–1 mA continuously (datasheet, three virtual sensors active).

**Fix direction — a product fork:**
- **Option A (wire the feature):** implement the intended raise-to-glance: a modest
  lv_timer sampling `hw_is_face_down()` edges → `glance_show()`. Keeps the IMU on
  with a reason. (Then PB.6 reduces to "suspend sensors during fake sleep".)
- **Option B (power-first):** call the (fixed) `hw_unregister_imu_process()` from
  `hw_power_down_all()` and re-register in `hw_power_up_all()`; optionally don't
  register at boot at all until a consumer exists.

**Risk:** Low (option B). ⚠️ HW: IMU-debug subpage still shows live data after a
sleep/wake cycle; re-register I2C cost (~50 ms) doesn't hurt wake feel.

#### PB.7 — NFC: full RF-field polling instead of ST25R3916 Wake-Up mode

**Where:** `hal/nfc_reader.cpp:664-666` — `totalDuration = 1000U`,
`discover_params.wakeupEnabled = false`; poll loop at `kPollMs = 20`
(`hal/nfc_task.cpp:33`). The RFAL state for WU mode is already handled in
`demoNotif()` (`nfc_reader.cpp:107`).

**Cost today:** only when the user enables NFC (default off — P2.2 already made the
disabled case free): continuous field-on polling ≈ 5–15 mA. WU mode (chip wakes the
host on amplitude/phase change) ≈ 75 µA — two orders of magnitude.

**Fix direction:** flip `wakeupEnabled = true` and let RFAL manage
WU-detect → full poll → back to WU; independently, `kPollMs` 20 → 100 is a free 5×
cut in lock traffic with no perceptible tap latency.

**Risk:** Med for WU mode (tag-tech coverage — verify type A/B/F + the NDEF flows in
`ui_nfc_test`); Low for the poll-interval bump. ⚠️ HW both.

### B. Awake-state churn (CPU/I2C/network cadences)

#### PB.8 — LVGL task wakes at 60 Hz with static content

**Where:** `hal/lvgl_task.cpp:41,67` — `lv_timer_handler()` returns the real next
deadline (≥ 33 ms when idle, `LV_DEF_REFR_PERIOD 33`), then
`if (next > kMaxTickMs) next = kMaxTickMs;` clamps every sleep to 16 ms.

**Cost today:** ~60 instance-mutex acquisitions/s on core 1 for nothing, and it's
the dominant contender against the keyboard/NFC tasks. The 16 ms cap predates input
moving to its own tasks — there is no responsiveness argument left: input events
arrive via queues/notifies, and `hw_lvgl_task_notify_wake()` already exists to kick
the task out of its sleep early.

**Fix direction:** honor `lv_timer_handler()`'s return (cap at ~200 ms for safety,
e.g. `kMaxTickMs = 200`), and audit that every input path that needs an immediate
frame kicks `hw_lvgl_task_notify_wake()` (keyboard/rotary already feed LVGL indevs —
verify the kick, not assume it). ⚠️ HW: scroll/typing feel, animation smoothness.

#### PB.9 — Keyboard awake-poll 100 Hz → 40–50 Hz

**Where:** `hal/keyboard_task.cpp:51` (`kPollMs = 10`; the in-code comment already
flags 20–30 ms as the open micro-item, carried from P2.1's "related" note).
Halves/thirds I2C + lock churn; no human-perceptible latency at 20–25 ms. ⚠️ HW:
fast-typing + auto-repeat feel. Tiny, separate commit.

#### PB.10 — Hub reachability probe every 10 s; the cache it feeds is 30 s ✅ done 2026-07-21

**Where:** `core/system.cpp:292-296` — `++hub_tick >= 10` on the 1 Hz status-bar
tick; consumers read `hub_last_reachable(max_age_ms = 30000)` (`hal/hub.h:58`).
Probing 3× faster than the freshness the consumers demand buys nothing; each probe
is a task spawn + TCP SYN (radio wake). **Fix:** `>= 10` → `>= 30`. Status-bar hub
icon lags ≤ 30 s instead of ≤ 10 s. Trivial.

**✅ Landed 2026-07-21:** `hub_tick >= 30` in `core/system.cpp`, with a comment
tying the cadence to the consumers' 30 s `max_age`. No HW risk (network cadence
only); builds green (hw + emu + native).

#### PB.11 — Telegram background poll cadence (product decision)

**Where:** `apps/ui_telegram.cpp:1871` — global 60 s lv_timer (created at boot,
never deleted; guards: app closed, internet cached-available, no text input, one
worker). Each firing that passes guards is a TLS round-trip (~1 s WiFi airtime).
Frozen during fake sleep (lv_timers pause), so this is an *awake* cost only.
**Options:** 60 s → 120–300 s (notification latency trade), or leave — flagging for
the product pass alongside P2.11/P2.12. No code smell here; the guards are right.

#### PB.12 — BLE keyboard: four small levers

**Where:** `hal/wireless.cpp` (BLE HID path). Verified state: full `deinit(true)` on
toggle-off ✅; keepalive task 1 Hz + 25 s HID heartbeat; iOS conn params
`updateConnParams(…, 0x18, 0x28, 0, 0x2A0)` (`wireless.cpp:44`).
1. **Slave latency 0 → ~10** (`wireless.cpp:44`): at a 30–50 ms interval the
   peripheral answers every connection event; latency 10 cuts radio wakes ~10× while
   idle-connected. Supervision timeout (6.72 s) comfortably covers it; Apple
   guidelines allow it. ⚠️ HW: keystroke latency + iOS reconnect stability.
2. **Advertising runs forever unconnected** (`hw_set_ble_kb_enable` →
   `bleKeyboard.begin()`): add a timeout (~5 min) + re-arm on keypress/toggle.
   Trade: host can't reconnect unprompted after the window — acceptable for HID.
3. **`ble_kb_ka` not fake-sleep gated** (`wireless.cpp:48-78`): 1 Hz wakes during
   sleep; mirror the notify-block pattern, kicked from `ui_resume_timers()`.
4. **TX power never set**: default +3 dBm; for a keyboard used at < 1 m,
   `NimBLEDevice::setPower()` down a step or two is worth a bench-check. (Also:
   the P3.14 TEMPORARY watermark print in this task is still live in the working
   tree — take the reading, land the stack trim, remove the print.)

#### PB.13 — Charge task: poll → PMU IRQ (fake-sleep's last periodic I2C)

**Where:** `hal/charge_task.cpp:144` — 500 ms `isVbusIn()` I2C poll, only during
fake sleep (awake it blocks on a notify; good design). After PB.4/PB.16 this becomes
the largest remaining sleep-state wake source. The BQ25896 has a VBUS-insert IRQ;
if the pager routes the PMU IRQ line (check `pins_arduino.h` / vendor init), moving
plug-detection to the ISR + notify zeroes the periodic cost. Med effort — only worth
it once the bigger rails are done and the meter confirms it matters.

#### PB.14 — Gauge sweep trims (= §1.5, with one correction to the record)

**Where:** `hal/power.cpp:300-332` inside `hw_get_monitor_params`. Re-verified all
five callers: they read **only** `.battery_percent` / `.is_charging` (plus
`.battery_voltage` internally). **Correction to the phase-1 record: `.temperature`
(L309) is also write-only** — `settings_info` uses `hw_get_battery_voltage()`
directly, nothing reads `.temperature`. That makes **9** removable
`instance.gauge.*` register reads per sweep (current, remaining/full/design
capacity, standby current, average power, max-load current, temperature,
timeToEmpty/Full). Keep: `refresh()`, `getStateOfCharge()`, `getVoltage()`,
`getBatteryStatus()` (gates `is_charging`), and the ppm `getFaultStatus()`
clear-side-effect noted in `OPTIMIZATION_PROGRESS.md`. Rides the hardware pass as
before; battery-relevant because it is live I2C every sweep (5 s idle / 1 s
charging).

#### PB.15 — `hw_get_battery_voltage()` bypasses the TTL cache ✅ done 2026-07-21

**Where:** `hal/power.cpp:79-83` — unconditional `gauge.refresh()` +
`getVoltage()`; called at 1 Hz by `apps/settings_info.cpp:57` while the info page is
open. `gauge.refresh()` is the *expensive* multi-register BQ28Z610 access the §2.12
TTL was added to throttle — this side door does it every second. **Fix:** route
through `hw_get_monitor_params()` and read `.battery_voltage` (≤ 5 s staleness on a
static info label is fine). Trivial.

**✅ Landed 2026-07-21:** the **caller** was fixed, not `hw_get_battery_voltage()`
itself — the function is also called from inside `hw_get_monitor_params()`
(`power.cpp:337`, gauge-offline fallback), so routing the function through the
monitor params would recurse. `settings_info.cpp:57` (the 1 Hz timer) now reads
`.battery_voltage` from the TTL-cached sweep and shares the status bar's cache. The
one-shot page-build read at `settings_info.cpp:185` was intentionally left on the
direct call (single read, freshest value at page open). Builds green.

### C. Micro / hygiene / adjacent correctness

#### PB.16 — Fake-sleep notify-blocks: 200 ms timeouts = 20 fallback wakes/s across 4 tasks

**Where:** `lvgl_task.cpp:56`, `keyboard_task.cpp:107`, `rotary_task.cpp:79`,
`nfc_task.cpp` — all `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200))`. The explicit
wake kicks (`hw_*_task_notify_wake()` from `ui_resume_timers()`) are the real wake
path; the 200 ms timeout is only a safety net. Raising to 1000 ms (or
`portMAX_DELAY` where the kick coverage is proven) cuts sleep-state scheduler wakes
4/5ths. Low risk — the kicks already exist and are on the smoke-test checklist; keep
the timeout non-infinite unless every wake path is audited.

#### PB.17 — CPU frequency set twice per fake-sleep transition

**Where:** `hw_power_down_all()` (`system.cpp:677`) and `hw_power_up_all()`
(`system.cpp:685`) each call `setCpuFrequencyMhz(…)`, then `factory.ino::loop()`'s
`last_freq` state machine (which doesn't know about those calls) repeats the set on
its next tick. Distinct from the boot-time P3.16 (fixed). **Fix:** let `loop()` own
frequency exclusively (accept ≤ 500 ms lag on entry), or update `last_freq` via a
setter. Cosmetic (~0.5 ms PLL re-lock each), but it keeps the frequency policy in
one place — worth doing when touching PB.20.

#### PB.18 — `hw_light_sleep()` is dead code with a latent security gap ✅ done 2026-07-21

**Where:** `hal/system.cpp:654-660` (+ decl `hal/system.h:60`). Zero callers
(`hw_low_power_loop()` calls `instance.lightSleep()` directly). Unlike `hw_sleep()`
/ `hw_low_power_loop()`, it does **not** `notes_crypto_lock()` or deinit audio
before suspending — any future caller would sleep with the notes passphrase in RAM.
**Fix:** delete it (phase-1 grep discipline: check `lib/LilyGoLib/src/` for extern
callers first), or harden the body to match `hw_sleep()`. Trivial either way.

**✅ Landed 2026-07-21:** deleted — grep confirmed zero callers in `src/` **and**
`lib/LilyGoLib/src/` (only the definition + decl existed). Removed both the body
(`hal/system.cpp`) and the declaration (`hal/system.h:60`). Chose delete over harden
so no half-safe sleep path can be resurrected by accident. Builds green.

#### PB.19 — Speaker amp: settings toggle latches the rail; playback path is fine (correcting the sweep)

**Verified mechanics** (this corrects an initial sweep finding before it entered the
record): on the pager (`USING_AUDIO_CODEC`), the vendor wires
`codec.setPaPinCallback` → `EXPANDS_AMP_EN` (`LilyGo_LoRa_Pager.cpp:472-474`), so
`codec.open()/close()` gates the amp per playback session — there is **no**
"amp on 24/7 during playback idle" and **no** "silent audio after fake sleep" bug.
What *is* true: `hw_set_speaker_enable(true)` (`hal/audio.cpp:614` →
`powerControl(POWER_SPEAK, en)`) latches the rail on until the next fake sleep cuts
it, independent of playback (2–8 mA amp quiescent). Default `speaker_enable = 0`, so
out-of-box cost is zero. **Fix (small, optional):** make the toggle a pure
preference gate (persist the flag; don't drive the rail), letting the codec callback
own the pin. Verify what the toggle is *meant* to do (keyboard clicks?) before
changing semantics — product check.

#### PB.20 — The 80 MHz fake-sleep CPU floor (compound with PB.2)

**Where:** `system.cpp:675-677` + the same logic in `factory.ino` —
`hold_80 = ble_connected || wifi_connected`. Correct today (the RF stacks want
≥ 80 MHz). After PB.2 (MAX_MODEM) the question becomes whether 40 MHz + modem-sleep
is stable — an ESP-IDF-version-dependent behaviour that must be answered by soak
test, not docs. ⚠️ HW-only item; park until PB.2 has bench numbers.

#### PB.21 — No ESP-IDF power management at all (investigate-only)

No `esp_pm_configure`, no tickless idle, no `CONFIG_PM_ENABLE` anywhere; the only
`esp_light_sleep_start()` path is the PMU-button one. Two investigation tracks, both
potentially bigger than everything above combined, both risky under Arduino:
1. **Auto light sleep** (`esp_pm_configure` + tickless): requires the Arduino
   framework's sdkconfig to have PM enabled (check before designing anything — the
   prebuilt core likely doesn't; a custom sdkconfig/IDF-component build is a big
   hammer).
2. **Periodic true light sleep during fake sleep:** from `factory.ino`'s fake-sleep
   branch, call the vendor `lightSleep()` with a timer + GPIO wake mask instead of
   `delay(500)` — the vendor already restores everything on wake. Wake sources are
   the crux (rotary center press, PMU IRQ, charger); the vendor path currently
   supports the boot button only, so this needs wake-source plumbing in vendored
   code. Sacrificial-device territory, like P3.25.

---

## 5. Verified clean this pass (don't re-sweep)

- Panel really sleeps (SLPIN) and backlight hits 0 in fake sleep; keyboard backlight
  rail cut. Haptic rail cut in sleep (DRV2605 idle cost only while awake, rail-gated
  by the setting).
- All lv_timers freeze during fake sleep (LVGL task blocks before
  `lv_timer_handler`); no app timer needs its own fake-sleep guard.
- Weather has **no** background poll (fetch on app open / tap; 15 min cache).
  Home-screen internet check is user-triggered. Menu badge/visibility timers are
  RAM-reads only and die with the menu.
- BLE fully deinits on toggle-off; audio player task blocks on its queue when idle
  (zero cost); recorder is on-demand; amp is codec-callback-gated (see PB.19).
- Charge task is notify-blocked while awake (only costs during sleep).
- GPS rail: force-off at boot, transient-only for GPS time sync (P2.5).
- Gauge sweep TTL (1 s charging / 5 s discharging) works as designed
  (`power.cpp:257-266`) — PB.15 is a bypass of it, not a flaw in it.

## 6. Worktree caveats at analysis time

- `src/hal/wireless.cpp` + `src/apps/ui_ssh.cpp` carry the **TEMPORARY P3.14/P3.9
  stack-watermark prints** — **now committed in `51c71a6`** (were uncommitted at
  analysis time), still mid-measurement from phase-3's 2026-07-16 session. The
  `ble_kb_ka` print itself wakes the serial path every 15 s; don't count it in any
  bench baseline, and finish those readings first. Removal is deferred until the
  P3.9/P3.14 stack readings are captured — the process is take-reading → land the
  stack trim → remove the print, so pulling the prints early forfeits both.
- `src/apps/ui_settings.cpp` carries the unrelated settings-shortcut feature diff
  (**also committed in `51c71a6`**) with a **known intermittent back-navigation
  crash** (flagged in P3.19's entry). Bench sessions that navigate settings should
  expect it.

## 7. Suggested execution order

1. **Trivial, zero-HW-risk first:** ✅ PB.10 (hub probe 10→30 s), ✅ PB.15 (voltage
   cache path), ✅ PB.18 (delete dead `hw_light_sleep`) — **all landed 2026-07-21**.
   PB.17 (freq ownership — optional) deferred to the PB.20 pass, where the finding
   itself says it belongs.
2. **The flagship:** PB.1 auto fake-sleep on `disp_timeout_second` (+ optional dim
   stage). Everything else multiplies through it.
3. **Fake-sleep gap batch (one bench session):** PB.4 (PPM ADC), PB.3 (radio sleep),
   PB.6 (IMU off in sleep + unregister fix), PB.2 (WiFi MAX_MODEM), PB.16 (notify
   timeouts) — each is small; measure the sleep-state current before/after each on
   the meter, in that order, and record deltas here. PB.5/P3.12 (SD rail) joins this
   session if its bench number justifies the remount work.
4. **Awake churn:** PB.8 (LVGL cap), PB.9 (keyboard poll) — feel-test on device.
5. **Product decisions:** PB.11 (telegram cadence), PB.7 (NFC WU mode), PB.12
   (BLE levers), PB.19 (speaker toggle semantics) — fold into the existing
   P2.11/P2.12 product-pass bucket.
6. **Deferred/investigate:** PB.13 (PMU IRQ), PB.20 (40 MHz floor), PB.21 (true
   light sleep) — only with bench data and a sacrificial device.

Per repo convention: one `<code>` + one `<docs>` commit per item; update this file's
items with ✅/measured-delta notes as they land; `OPTIMIZATION_PROGRESS.md`'s
methodology stays in force — **re-verify every file:line here against current source
before editing; this file will go stale the same way its three predecessors did.**

---

## Bench appendix — the baseline table to fill on the next hardware session

Measure at the battery (or USB with charging complete/disabled), 30 s settle per row:

| State | Setup | mA (measured) |
|---|---|---|
| Active, menu, full backlight, WiFi on | default boot | |
| Active, menu, WiFi off, BLE off | toggles off | |
| Idle-awake > 2 s (80 MHz step) | hands off | |
| Fake sleep, WiFi connected | long-press | |
| Fake sleep, WiFi off, BLE off | | |
| Fake sleep after PB.4 / PB.3 / PB.6 / PB.2 (one row each, cumulative) | | |
| Vendor light sleep (PMU button) | reference floor | |
| NFC enabled, idle (field on) | for PB.7 | |

The "vendor light sleep" row is the target floor for the fake-sleep column: every mA
of gap is exactly the PB.2–PB.6 list.
