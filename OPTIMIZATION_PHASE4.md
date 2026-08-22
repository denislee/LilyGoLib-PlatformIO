# Optimization Phase 4 — Battery & General Analysis

Generated **2026-08-22** against a clean tree at **`aa67d2e`** ("land the PB battery
pass"). This is the first sweep taken *after* the `PB.x` battery batch landed, so its
first job is to re-verify that batch in tree and its second is to find what the batch
did not cover.

Findings are numbered **`P4.x`**, continuing the `P2.x`/`P3.x` series. Battery items
that supersede or extend a `PB.x` item name it explicitly.

**Relationship to the other docs**
- `OPTIMIZATION_PROGRESS.md` — phase-1 tracker. Its **Methodology** (how to prove a
  function is dead) and its **hardware smoke-test checklist** apply verbatim here.
- `OPTIMIZATION_PHASE2.md` / `OPTIMIZATION_PHASE3.md` — phases 2–3.
- `OPTIMIZATION_BATTERY.md` — the `PB.x` battery handoff. **Still the canonical record
  for PB.1–PB.21.** This file does not restate them; §1 records only where the tree
  disagrees with that doc, and the new items cross-reference it.

> **⚠️ Measurement discipline (inherited, unchanged).** Every mA figure below is a
> datasheet/first-principles **estimate**. Nothing in this codebase has ever been
> current-measured. The `PB.x` batch shipped unmeasured too — so the highest-value
> action available today is still *the bench session*, not another code change. Fill
> `OPTIMIZATION_BATTERY.md`'s **Bench appendix** before investing in anything tagged
> Med/High effort here, and do not "fix" an item whose measured cost is noise.

> **🔴 BATCH LANDED UNMEASURED — 2026-08-22.** Items P4.1–P4.30 were implemented and
> all three build targets passed (`tlora_pager`, `emulator_lora_pager`, `native_test`,
> 28/28 tests green). **No current measurements were taken before or after this pass.**
> Every mA figure in this document remains a first-principles estimate. The bench session
> is still the highest-value action available. Do not reprioritise open items based on
> the estimates alone — measure first. Status legend: ✅ landed · 🔶 partial (see item
> for open half) · ⏸ deferred (bench/HW/product-gated).

**Build discipline (unchanged):** after every change,
`pio run -e tlora_pager && pio run -e emulator_lora_pager && pio test -e native_test`.
Anything touching task loops / ISRs / rails / sleep paths is ⚠️ **hardware-test-required**.

---

## 1. State of the `PB.x` batch in tree (verified, not trusted)

Spot-checked against `aa67d2e`. Everything claimed landed **is** in tree:

| Item | Claim | Verified at |
|---|---|---|
| PB.1 | auto fake-sleep on `disp_timeout_second` | `src/factory.ino:236-245` ✅ — **but see P4.1** |
| PB.2 | WiFi PS MIN→MAX_MODEM across sleep | `hal/wireless.cpp:177-194`, called `hal/system.cpp:711,751` ✅ — **but see P4.17** |
| PB.3 | radio slept on sleep entry / `RADIO_DISABLE` | `hal/system.cpp:701-704`, `hal/radio/sx1262.cpp:74` ✅ — **but see P4.18** |
| PB.4 | `ppm.disableMeasure()` in fake sleep | `hal/system.cpp:681` ✅ — **but see P4.7** |
| PB.6 | all three BHI260 sensors suspended | `hal/sensors.cpp` `hw_unregister_imu_process()` ✅ — **but see P4.6, P4.22** |
| PB.8 | LVGL honours `lv_timer_handler()` deadline, cap 200 ms | `hal/lvgl_task.cpp:52,78` ✅ — **but see P4.10** |
| PB.9 | keyboard poll 10 → 20 ms | `hal/keyboard_task.cpp:51` ✅ — **but see P4.12** |
| PB.10 | hub probe 10 s → 30 s | `core/system.cpp:296` ✅ |
| PB.12 | slave latency 10, TX power 0 dBm, ka fake-sleep gated | `hal/wireless.cpp:49,67-71`, enable path ✅ |
| PB.15 | info page reads through the TTL cache | `apps/settings_info.cpp:57-62` ✅ (the residual `hw_get_battery_voltage()` at `:190` is a one-shot page-build read — correct as-is) |
| PB.16 | notify timeouts 200 ms → 1000 ms | `lvgl_task.cpp:40`, `keyboard_task.cpp:61`, `rotary_task.cpp:~52`, `nfc_task.cpp:37` ✅ |
| PB.17 | single `hw_set_cpu_freq()` owner | `factory.ino:73,85,206,226-228`, `hal/system.cpp:695,725` ✅ |

One documented-as-deferred item is worth re-reading in light of what follows: **PB.13**
(charge-task VBUS poll) was closed as "not implementable — the pager doesn't route the
BQ25896 /INT to a GPIO". That is correct, but it foreclosed the *other* half of the
lever — see **P4.8**.

**Worktree caveat resolved:** ✅ the P3.14 `[stackwm]` watermark print in
`hal/wireless.cpp:72-77` has been removed (P4.14 landed 2026-08-22). BLE bench rows are
no longer poisoned by the instrument.

---

## 2. Executive summary — new findings

The one-line version: **the `PB.x` batch built the sleep machinery correctly and then
shipped it switched off, and the awake state still guarantees a screen redraw every
second forever.** Those two items (P4.1, P4.10) dominate everything else in this file.

| # | Finding | Est. impact | Risk / effort |
|---|---|---|---|
| **P4.1** ✅ | `disp_timeout_second` **defaults to 0 = "Always"** — PB.1's auto-sleep never fires on a fresh device | Nullifies the entire PB batch for any user who never opens Settings › Display | Trivial / product call |
| **P4.10** ✅ | Status bar rewrites its labels at 1 Hz although the text changes ≤ 1×/min — a forced dirty-rect + SPI flush every second, forever | Defeats PB.8; keeps CPU+SPI busy on a static screen | Low / low risk |
| **P4.2** 🔶 | Fake-sleep entry runs `hw_power_down_all()` **twice** and rebuilds the whole home menu **at 40 MHz with the panel already asleep** | CPU burst + ~10 redundant I2C writes on *every* sleep entry — now once per timeout, not once per long-press | Low code / ⚠️ HW |
| **P4.15** ⏸ | Every HTTPS request constructs a fresh `WiFiClientSecure` → **full TLS handshake per request**, no session reuse, no keep-alive | ~1–2 s of RF + ECDHE per poll; compounds P4.16 | Med / ⚠️ HW |
| **P4.3** ✅ | No dim stage before sleep, and the shipped default is **maximum backlight** (50 clamps to 16/16) | Backlight is the largest awake consumer on this board | Low / product call |
| **P4.16** ✅ | Telegram 60 s poll costs an estimated ~2 mA *average* while awake (arithmetic below) — PB.11 closed this as "product", without the number | Largest single awake network cost | Product call |
| **P4.11** ✅ | Vendor rotary task polls GPIO at **500 Hz** awake, at priority 10 (above LVGL's 8) | Blocks any future tickless idle; steady CPU floor | Low (backoff) → Med (PCNT) / ⚠️ HW |
| **P4.4** ⏸ | Fake sleep never deepens: WiFi stays associated indefinitely, no ladder to true light sleep | Standby floor stays at the PB.2–PB.6 level forever | Med / ⚠️ HW |
| **P4.22** ✅ | BHI260 runs 3 virtual sensors whenever awake for a debug page and a `glance_show()` with **zero call sites** | ~0.5–1 mA whenever awake | Low / product call |
| **P4.6** ✅ | Every wake re-dumps the BHI260 sensor table to Serial and walks 255 sensor IDs | ~50 ms + UART traffic per wake, now per timeout | Trivial |
| **P4.17** ✅ | WiFi enabled-but-not-associated is never retried and never powered down; PS escalation is skipped in that state | Modem powered for zero connectivity, indefinitely | Low–Med |
| **P4.12** ✅ | Keyboard polls at 50 Hz although the TCA8418 IRQ is wired and an ISR is already attached | 50 instance-mutex takes/s awake | Med / ⚠️ HW |
| **P4.20** ⏸ | Pager renders into **two full-screen PSRAM buffers** (~426 KB) and flushes with a blocking CPU-driven SPI write — `useDMA()` is false for every board but the Ultra | More CPU-ms per frame than needed; compounds P4.10 | Med / ⚠️ HW, internal-RAM trade |
| **P4.5** ✅ | The 80 % charge cap is enforced from an `lv_timer` → **frozen during fake sleep** | Feature silently inoperative in the state the device lives in | Low / correctness |
| **P4.18** ⏸ | The LoRa TX/RX path is dead: enabling the radio configures it and immediately sleeps it | Dead code + a settings toggle that does nothing; and a *pager* that cannot be paged | Product call |
| **P4.23** ⏸ | TinyUSB + OTG PHY unconditionally on (`ARDUINO_USB_MODE=0`) even on battery | est. 1–3 mA, 24/7 | Investigate / ⚠️ HW |
| **P4.8** ✅ | Charge task polls VBUS at a flat 2 Hz for the whole sleep — the last periodic I2C in sleep | small, but it is the floor | Trivial |
| **P4.7** ✅ | Charge overlay reads the PPM while PB.4 has its ADC disabled → stale/zero USB & sys voltage cached for ≤ 5 s | correctness regression from PB.4 | Trivial |
| **P4.14** ✅ | `ble_kb_ka` polls at 1 Hz awake and still carries the P3.14 `[stackwm]` print | small + poisons any bench baseline | Trivial |
| **P4.13** ✅ | Badge label rewritten every 2 s; Info page reads the RTC over I2C at 1 Hz | I2C + redraw churn while those views are open | Trivial |
| **P4.19** ✅ | NTP retry loop re-fires every 30 s forever on failure, and can drag an HTTP timezone lookup with it | repeated RF wakes in the failure case | Low |
| **P4.9** ✅ | Auto-sleep has no external-power / USB-MSC guard | product behaviour | Trivial / product call |
| **P4.21** ✅ | `DEVICE_MAX_BRIGHTNESS_LEVEL` defined twice with different values (255 vs 16); include order decides | latent — slider range depends on header order | Trivial / correctness |
| **P4.24** ✅ | 114 raw `printf`/`Serial.print` sites survive `CORE_DEBUG_LEVEL=0`; libc `printf` blocks on UART0 (~87 µs/char) | ms-scale stalls, always compiled in | Low |
| **P4.27** ✅ | `setInsecure()` on **every** HTTPS path — no certificate validation anywhere, including the Telegram bearer token | security | Med |
| **P4.28** ✅ | WiFi password printed to Serial in cleartext | security | Trivial |
| **P4.29** ✅ | `hw_power_down_all()` never calls `notes_crypto_lock()` — only the PMU-button paths do | security; auto-sleep is now the *common* sleep path | Trivial / product call |
| **P4.30** ✅ | Battery-history ring sized against a 5-min interval but driven at 60 s → holds 1 h, not 5 h | cosmetic/correctness | Trivial |
| **P4.25** ⏸ | `LV_USE_DRAW_SW_ASM = NONE`; no ESP32-S3 SIMD blend path | render CPU | Investigate |
| **P4.26** ⏸ | `playerTask` 8 KB internal stack reserved at boot for an idle queue-blocked task (= P2.6, still open) | internal RAM | Low |

---

## 3. Detailed findings

### A. The sleep/wake state machine

#### P4.1 ✅ — `disp_timeout_second` defaults to **0 = "Always"**: PB.1 shipped switched off (HIGHEST VALUE)

**✅ Landed 2026-08-22:** `disp_timeout_second` defaulted to `60` s in both Arduino and emulator blocks (`hal/system.cpp:382,457`); `brightness_level` default lowered from `50` to `8` (Arduino `:380`, emulator `:455`); `SETTINGS_VERSION` bumped to `13`, causing a wholesale reset-to-defaults on devices with the previous blob (correct documented behaviour for breaking schema changes).

`OPTIMIZATION_BATTERY.md` calls PB.1 "the flagship" and "the largest real-world lever",
and the mechanism did land — `src/factory.ino:236-245` fires
`lilygo_request_fake_sleep_toggle()` once `lv_display_get_inactive_time()` clears the
setting. But the setting's default is zero:

```c
// src/hal/system.cpp:363   (hardware defaults)
user_setting.disp_timeout_second = 0;
// src/hal/system.cpp:438   (emulator defaults)
user_setting.disp_timeout_second = 0;
```

and the guard is `cached_disp_timeout_sec > 0` (`factory.ino:237`), with the UI
rendering `0` as `" Always "` (`apps/settings_display.cpp:238-239`).

So on a freshly-flashed device — and on any device whose owner has never visited
Settings › Display and moved a slider — **the behaviour is byte-for-byte the pre-PB.1
behaviour: the device never sleeps on its own.** Every downstream saving in the PB
batch (PB.2/3/4/6, and the whole fake-sleep gap list) multiplies through a state the
device only reaches by a deliberate 1-second button hold.

**Fix.** Pick a non-zero default — `30` or `60` s is the phone-like choice; `120` s is
the conservative one — and add a settings-migration note so existing devices with a
stored blob also pick it up (the versioned-blob path at `system.cpp:405-419` keeps the
old value, so a `SETTINGS_VERSION` bump is the honest way to re-default in the field).

**Coupled UX call, decide before defaulting:** once asleep the *only* wake is a >1 s
centre-button hold — the keyboard rail is cut (`system.cpp:687-688`) and rotary scroll
is discarded (`LilyGo_LoRa_Pager.cpp:1375-1377`). A 30 s default plus a 1 s hold to
resume is a real feel change. Consider pairing the default with a cheaper wake (any
rotary notch, or a short click) before shipping it.

*Impact:* the difference between the PB batch mattering and not mattering.
*Effort:* one line + a version bump. *Risk:* product, not technical.

---

#### P4.2 🔶 — Fake-sleep entry powers down twice and rebuilds the home menu at 40 MHz with the panel already off

**🔶 Partial — 2026-08-22:** The idempotency half landed: `ui_pause_timers()` now guards on a `static bool s_power_down_active` flag (`src/ui_main.cpp:22,76-80`) and returns early on a second call, eliminating the redundant `hw_power_down_all()`. `ui_resume_timers()` clears the flag on wake (`ui_main.cpp:55`). The reorder (moving `ui_request_editor_switch()` before `ui_pause_timers()` so the menu rebuild happens before the rails drop) was **skipped** — the vendor `perform_fake_sleep_toggle()` holds `ui_lock()` around the sequence and `showMenu()` acquires the LVGL instance lock, making the ordering non-trivial to reason about with two nested locks. The redundant double-powerdown is eliminated; the wasted render-at-40-MHz remains. That half can be revisited once the lock ordering is fully mapped.

The vendor toggle does two things in this order:

```c
// lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp:1303-1312
instance.setBrightness(0);
instance.sleepDisplay();
s_display_off = true;
ui_lock();
ui_pause_timers();            // -> hw_power_down_all()
ui_request_editor_switch();   // <-- and then this
ui_unlock();
```

and `ui_request_editor_switch()` (`src/ui_main.cpp:38-45`) *clears the fake-sleep flag*
and schedules a 10 ms LVGL timer:

```c
void ui_request_editor_switch() {
    fake_sleep_active = false;      // every parked task un-blocks here
    hw_lvgl_task_notify_wake();     // and the LVGL task is explicitly kicked
    lv_timer_create(deferred_switch_timer_cb, 10, NULL);
}

static void deferred_switch_timer_cb(lv_timer_t *t) {   // ui_main.cpp:18-36
    ...
    core::System::getInstance().showMenu();   // full menu rebuild
    lv_display_trigger_activity(NULL);
    ui_pause_timers();                        // -> hw_power_down_all() AGAIN
    lv_timer_del(t);
}
```

Consequences on **every** sleep entry:

1. **`hw_power_down_all()` runs twice.** Second pass re-issues `ppm.disableMeasure()`,
   five `instance.powerControl()` expander writes, `kb.end()`, `radio_chip::sleep()`
   under the SPI lock, `hw_wifi_powersave_sleep()`, and `hw_unregister_imu_process()`
   (three more I2C `configure()` calls). ~10+ redundant I2C transactions plus an SPI
   round-trip, for nothing.
2. **The home menu is rebuilt with the display already asleep** — every tile, label,
   font measurement and layout pass, rendered into a buffer that is never shown.
3. **It is rebuilt at 40 MHz**, because the first `hw_power_down_all()` already dropped
   the clock (`hal/system.cpp:694-695`). Same work, ~6× the wall-clock, and the CPU is
   awake for all of it.
4. For that window every notify-parked task (LVGL, keyboard, rotary, NFC, charge,
   `ble_kb_ka`) sees `ui_is_fake_sleep() == false` and resumes full-rate polling.

This was tolerable when sleep entry meant "the user deliberately held a button". With
P4.1 fixed it becomes a per-timeout cost.

**Fix.** Reorder so the app switch happens *before* the power-down, and pause exactly
once. Cleanest shape without touching vendor code: have `ui_pause_timers()` become
idempotent (guard on `fake_sleep_active`) **and** move the menu switch ahead of the
rail teardown — e.g. make `ui_request_editor_switch()` do the `showMenu()` synchronously
and let a single `ui_pause_timers()` follow it. The 10 ms deferral exists to get the
switch off the vendor's call stack; a direct call from the vendor hook order
(`ui_request_editor_switch()` **then** `ui_pause_timers()`) removes the need for it.

*Impact:* removes a CPU/I2C burst and a wasted full render per sleep entry.
*Effort:* Low. *Risk:* ⚠️ HW — this is the sleep path; smoke-test wake, editor-exit and
the charge-overlay round trip.

---

#### P4.3 ✅ — No dim stage, and the shipped default is maximum backlight

**✅ Landed 2026-08-22:** Both halves done. Default `brightness_level` lowered `50→8` (landed alongside P4.1). Dim ladder added to `src/factory.ino:248-295`: at 60 % of `disp_timeout_second` the display dims to `max(1, brightness/3)` and keyboard LED to `max(1, kb_brightness/3)`; restores immediately on LVGL activity; write-caching skips AW9364 calls when the value is unchanged; `s_dimmed` flag cleared on auto-sleep-pending (hardware restore happens in `hw_power_up_all()` on wake).

Two separate facts that compound:

1. **Default brightness is max.** `user_setting.brightness_level = 50`
   (`hal/system.cpp:361`), but the pager's AW9364 driver clamps at 16 steps:
   ```c
   // .pio/libdeps/tlora_pager/SensorLib/src/AW9364LedDriver.hpp:62-64
   if (value > 16) value = 16;
   ```
   So the factory default is **16/16 — full brightness**. (Keyboard backlight defaults
   to `80/255` ≈ 31 %, `system.cpp:362`.)
2. **There is no dim stage anywhere.** `factory.ino` has exactly one idle behaviour
   before sleep — the 80 MHz CPU step at `inactive_time > 2000` (`factory.ino:225-229`).
   The backlight goes from full to zero, in one jump, at the timeout.

On a board of this class the backlight is almost certainly the single largest awake
consumer — larger than the CPU, the modem in PS, and every I2C peripheral combined.
Every second the device spends idle-awake at 16/16 is the most expensive second it has.

**Fix (both halves):**
- Lower the default to roughly half-scale (`8`), which on this driver is a genuine
  ~50 % duty reduction, and let the user raise it.
- Add a dim stage to the same `factory.ino` idle ladder that already owns the CPU step:
  at ~50–60 % of `disp_timeout_second`, `hw_set_disp_backlight(max(1, level/3))` and
  restore on the next `lv_display_trigger_activity()`. Keep the keyboard backlight on
  the same ladder — it is a second LED rail with the same profile.
- Note the driver's own quirk when implementing: `AW9364LedDriver::setBrightness()`
  early-returns on `_brightness == value` **before** clamping, and steps the level by
  pulsing the enable pin — restoring brightness is not free, but it is microseconds.

*Impact:* likely the largest awake-state saving available. *Effort:* Low.
*Risk:* product (people dislike surprise dimming) — pair it with a Settings toggle.

---

#### P4.4 ⏸ — Fake sleep never deepens: no escalation ladder

**⏸ Deferred — needs bench data and product call.** The two-tier ladder requires measuring how much the WiFi modem actually costs in `WIFI_PS_MAX_MODEM` (the bench row is in §6 below) and a product decision about the reconnect delay users will see on wake. Do not implement Tier 2 until the bench delta is in hand and the UX trade-off is decided.

Fake sleep is a single flat state. Once entered, it stays exactly as entered for as
long as the device sits there — days, if it must. In particular:

- **WiFi remains associated indefinitely**, at `WIFI_PS_MAX_MODEM` (PB.2). MAX_MODEM
  reduces the beacon-listen duty cycle, it does not stop the modem: the device still
  wakes the RF front-end on its listen interval, forever, to receive traffic that
  nothing will consume — every `lv_timer` is frozen, so the Telegram poll, the hub
  probe and the weather cache are all dead until wake.
- **CPU is pinned at 80 MHz whenever WiFi or BLE is up** (`factory.ino:205-206`,
  `hal/system.cpp:694-695`) — that is PB.20, still deferred, and it is a *consequence*
  of keeping the links alive rather than an independent knob.
- **The device never reaches the vendor's own light-sleep floor** on its own. The
  vendor `lightSleep()` path (`LilyGo_LoRa_Pager.cpp:678-764`) — the reference floor
  the battery doc names — is reachable only by a PMU-button long-press.

**Fix — a two-tier ladder inside fake sleep.** The charge task is already awake in
fake sleep on its own timer (`hal/charge_task.cpp:144`), so it is the natural place to
age the state without adding a task:

- **Tier 1** (now): current behaviour.
- **Tier 2** (after N minutes asleep, N ≈ 5–15): drop the links —
  `WiFi.disconnect(true); WiFi.mode(WIFI_OFF)` and `hw_set_ble_kb_disable()` — which
  also *unblocks PB.20*, since the 80 MHz floor exists only to keep those links up.
  Restore both from `hw_power_up_all()` on wake (the reconnect helper already exists:
  `hw_wifi_reconnect_saved()`, `hal/wireless.cpp:158`).
- **Tier 3** (optional, needs a sacrificial device): after a much longer idle, hand off
  to the vendor `instance.lightSleep(WAKEUP_SRC_BOOT_BUTTON)`.

The cost is honest and must be stated in the UI: Tier 2 means a woken device needs a
few seconds to re-associate before Telegram/hub/weather work. Tier 3 changes the wake
button. Both are product calls, not silent changes.

*Impact:* this is where the standby floor actually moves — potentially the largest
sleep-state saving left. *Effort:* Med. *Risk:* ⚠️ HW + product.

---

#### P4.5 ✅ — The 80 % charge cap is enforced from an `lv_timer`, so it is frozen during fake sleep

**✅ Landed 2026-08-22:** Added `kBatteryCheckMs = 60000` constant and a `last_battery_chk_ms` tracker in `src/hal/charge_task.cpp`. Inside the fake-sleep poll loop, once per minute the task takes `core::ScopedInstanceLock` and calls `hw_update_battery_history()`. The function contains no LVGL calls (gauge/PPM I2C reads and `hw_set_charger()` only), so it is safe to invoke from the charge task. The awake `lv_timer` callback is unchanged — it continues to drive the cap while the screen is on.

The charge-conservation feature lives in `hw_update_battery_history()`
(`hal/power.cpp:36-55`) — it is what stops the charger at 80 % and re-enables it below
75 %. Its only driver is an LVGL timer:

```c
// src/hal/system.cpp:313
lv_timer_create(battery_history_timer_cb, 60000, NULL);
```

Every `lv_timer` is de-facto frozen in fake sleep — `lv_timer_handler()` is never
called because the LVGL task parks at `hal/lvgl_task.cpp:62-68`. So the cap is enforced
**only while the screen is on**.

The realistic failure is the ordinary one: plug the device in at night, it auto-sleeps
(P4.1 once fixed), and it charges straight past 80 % to full — exactly the scenario the
feature exists to prevent.

**Fix.** Move the check off LVGL. The charge task is already running in fake sleep and
already samples VBUS at 2 Hz; give it a slow secondary tick (once a minute is ample)
that calls `hw_update_battery_history()` when `ui_is_fake_sleep()`. Keep the LVGL timer
for the awake case, or drive both from the charge task and delete the `lv_timer`.

Related, same function: **P4.30** ✅ — `MAX_BATTERY_HISTORY = 60` comment corrected from "5 hours if 5 mins interval" to "1 hour at 60 s interval" (`hal/power.cpp:21`). The ring size (60) and timer interval (60 s) were not changed — comment was the only error. If 5-hour history is genuinely desired, the constant would need to change to 300 (~600 bytes additional RAM), which is a behavioural change deferred per task instructions.

*Impact:* correctness, not mA — but it is a battery-*health* feature failing silently.
*Effort:* Low.

---

#### P4.6 ✅ — Every wake re-dumps the BHI260 sensor table to Serial and walks 255 sensor IDs

**✅ Landed 2026-08-22:** Heavy diagnostics (`printInfo` + 255-ID `bhy2_is_sensor_available` walk) removed from `hw_register_imu_process()` and placed in a new `hw_probe_imu_info()` function (`src/hal/sensors.cpp` ~line 212). Declaration added to `src/hal/sensors.h`. The IMU debug page's `build_subpage()` calls `hw_probe_imu_info()` after `hw_register_imu_process()` — probe is now lazy, per-page-open. Note: with P4.22 also landed, `hw_register_imu_process()` is no longer called at all from the wake path (see P4.22), so this cost is gone from wake entirely.

`hw_power_up_all()` calls `hw_register_imu_process()` (`hal/system.cpp:756`), and that
function opens with pure diagnostics:

```c
// src/hal/sensors.cpp:153-166
BoschSensorInfo info = instance.sensor.getSensorInfo();
info.printInfo(Serial);                       // full table dump, every wake
uint16_t count = 0;
if (info.dev) {
    for (uint8_t id = 1; id < BHY2_SENSOR_ID_MAX; ++id) {
        if (bhy2_is_sensor_available(id, info.dev)) count++;   // 255 iterations
    }
}
s_imu_diag.sensor_count = count;
```

`s_imu_diag.sensor_count` feeds one line on the IMU debug page. The Serial dump feeds
nobody. The battery doc already notes this path is "~50 ms — acceptable for wake feel";
that judgement was made when wake happened once per deliberate button press. With P4.1
fixed it happens on every timeout cycle, and the ~50 ms is spent at whatever clock the
wake path has reached.

**Fix.** Split the function: keep the three `configure()` + `onResultEvent()` pairs in
`hw_register_imu_process()`, and move the `printInfo` + availability walk into a
`hw_probe_imu_info()` called once from `hw_init()` (or lazily, from the IMU debug page's
`build_subpage`). See also **P4.22** — if the IMU stops being registered at boot at all,
this cost disappears from the wake path entirely.

*Effort:* Trivial. *Risk:* Low.

---

#### P4.7 ✅ — The charge overlay reads the PPM while PB.4 has its ADC disabled

**✅ Landed 2026-08-22:** `cached_params`/`last_refresh` promoted from local statics to file-level `s_cached_monitor_params`/`s_monitor_last_refresh` in `src/hal/power.cpp`; `hw_invalidate_monitor_cache()` added (declared in `power.h`). In `src/hal/charge_task.cpp`: wakeup block calls `hw_invalidate_monitor_cache()` then `instance.ppm.enableMeasure()` before `build_charge_overlay()` (both guarded by `#if defined(USING_PPM_MANAGE)`); teardown block calls `instance.ppm.disableMeasure()` after `instance.sleepDisplay()` to restore the power-down state.

PB.4 added `instance.ppm.disableMeasure()` to `hw_power_down_all()`
(`hal/system.cpp:681`) on the correct reasoning that nobody reads the gauge while LVGL
is blocked. But something does: the charge overlay, which exists precisely to fire
during fake sleep.

```c
// src/hal/charge_task.cpp:65-68
lv_obj_t *build_charge_overlay() {
    monitor_params_t p;
    hw_get_monitor_params(p);      // BQ25896 ADC is off at this moment
```

`hw_get_monitor_params()` (`hal/power.cpp:~251+`) reads `getVbusVoltage()` and
`getSystemVoltage()` from the BQ25896 — both ADC-backed — and then **caches the whole
struct** for the next 1–5 s (`cached_params = params`). The visible symptom is mild
(the overlay itself only renders `battery_percent`, which comes from the separate
BQ27220 gauge, and `isVbusIn()` is a status-register bit, so detection still works),
but the stale zeros land in the cache and can surface on the Info page for up to 5 s
after wake.

**Fix.** In the overlay path, `ppm.enableMeasure()` before the read and
`ppm.disableMeasure()` after — or simply re-enable it as part of the
`instance.wakeupDisplay()` block at `charge_task.cpp:163-170`, since the device is
briefly awake there anyway, and re-disable it in the teardown block at `:176-182`.

*Effort:* Trivial. *Risk:* ⚠️ HW (verify the overlay still shows a sane percentage).

---

#### P4.8 ✅ — Charge task polls VBUS at a flat 2 Hz for the entire sleep

**✅ Landed 2026-08-22:** Replaced the single `kPollMs = 500` constant with a three-tier scheme in `src/hal/charge_task.cpp`: `kPollFastMs = 500` (first 30 s after sleep entry or VBUS rising edge), `kPollMediumMs = 2000` (30–90 s), `kPollSlowMs = 5000` (90 s+). A `last_reset_ms` variable is reset on fake-sleep entry and on every VBUS rising edge; the per-iteration poll delay is derived from `millis() - last_reset_ms`.

`kPollMs = 500` (`hal/charge_task.cpp:37`), unconditional for as long as fake sleep
lasts. This is the **last periodic I2C transaction in the sleep state** — every other
task is notify-parked with a 1 s safety-net timeout, and `loopTask` is at 2 Hz doing an
EventGroup read.

PB.13 correctly established that the IRQ-driven version is not available (the pager
does not route the BQ25896 `/INT`). But polling cadence is still a free variable, and
nothing about plugging in a cable is latency-critical: a 2 s detection delay before a
"Charging 47 %" overlay appears is imperceptible.

**Fix.** Progressive backoff keyed on time-in-sleep: 500 ms for the first ~30 s after
sleep entry (so the common "sleep, then immediately plug in" flow stays snappy), then
2 s, then 5 s. Reset on every wake. Roughly a 4–10× reduction in the only remaining
periodic wake in the deepest state the device currently reaches.

*Effort:* Trivial. *Risk:* Low (⚠️ HW to confirm the overlay still fires).

---

#### P4.9 ✅ — Auto-sleep has no external-power or USB-MSC guard

**✅ Landed 2026-08-22, with the product call made explicitly.** Both guards are in the
auto-sleep condition in `src/factory.ino`: `hw_is_usb_msc_mounted()` and an
`is_on_external_power()` helper wrapping `hw_get_monitor_params().is_charging`. Both are
evaluated **last** in the `&&` chain, so the TTL-cached PMIC read only happens on a tick
that would otherwise have slept — not at the loop's 20 Hz.

> **Product decision — "do not auto-sleep while charging" was chosen deliberately.**
> A docked device stays awake. The cost, stated plainly because it is not obvious:
> this **cancels P4.5's motivating scenario**. P4.5 exists so the 80 % charge cap keeps
> working overnight while asleep — but with this guard, a device plugged in *before* it
> idles never reaches fake sleep at all. P4.5 now only covers the sleep-then-plug-in
> order. If the overnight case matters more than the dock case, this guard is the thing
> to remove, and P4.5 immediately starts earning its keep.

The auto-sleep guard list (`factory.ino:236-242`) covers audio playback, recording,
text-input focus and SSH. It does not consider:

- **VBUS present / charging.** Most devices in this class do not auto-sleep on external
  power. The state is already available via `hw_get_monitor_params().is_charging`.
- **USB MSC mounted.** `hw_is_usb_msc_mounted()` exists and is read by the status bar
  (`core/system.cpp:335`); the device will happily blank the screen mid-transfer. The
  SD rail stays powered through fake sleep so the transfer itself survives — this is a
  UX wart, not a corruption risk, but the "Unsafe to disconnect" overlay it is fighting
  with is on `lv_layer_top()` and will be invisible.

**Fix.** Two more clauses in the same `if`. Whether "no sleep while charging" is the
right default is a product call — state it either way rather than leaving it implicit.

*Effort:* Trivial.

---

### B. Awake-state churn

#### P4.10 ✅ — The 1 Hz status bar forces a redraw + SPI flush every second, forever

**✅ Landed 2026-08-22:** Cache-and-compare added before every `lv_label_set_text*` on all periodic paths. In `src/core/system.cpp`: static cache vars (`prev_clock_min/hour/mday/mon/year`, `prev_batt_pct`, `prev_is_charging`, `prev_file_count`, `prev_mem_free_kb`) added inside the 1 Hz timer lambda; clock and battery labels only written when displayed values change; caches reset when header font changes. In `src/apps/menu_app.cpp` (P4.13a): `s_badge_cached_counts` vector skips `lv_label_set_text` when count is unchanged. In `src/apps/settings_info.cpp` (P4.13b): RTC read throttled to every 5th tick; RSSI and battery-voltage labels only written on change.

This is the awake-state counterpart to P4.1, and it quietly undoes PB.8.

PB.8's whole premise (`hal/lvgl_task.cpp:41-52`) is that a static screen lets
`lv_timer_handler()` return a long deadline, so the LVGL task can sleep up to 200 ms
instead of waking at 60 Hz. But the status-bar timer (`core/system.cpp:180-433`)
rewrites its labels unconditionally on every tick:

```c
// core/system.cpp:229 — minute resolution, rewritten 60×/minute
lv_label_set_text_fmt(self._statTimeLabel, "%02d/%02d/%04d %02d:%02d", ...);
// core/system.cpp:237 — percent changes every several minutes at best
lv_label_set_text_fmt(self._statBattLabel, "%s %d%%", ...);
```

`lv_label_set_text*` in LVGL 9 copies the string and calls `lv_label_refr_text()` →
`lv_obj_invalidate()` **unconditionally**; it does not compare against the current
text. So 59 out of every 60 time-label updates, and essentially all battery-label
updates, dirty a region for a string that did not change. LVGL then re-renders that
region and `disp_flush()` pushes it over SPI (`LV_Helper_v9.cpp`, `disp_flush`).

Net: **the screen is never static.** The LVGL task's 200 ms cap is unreachable, the
draw pipeline runs every second, and on this board the flush is a blocking CPU-driven
SPI write out of PSRAM (see P4.20) — the most expensive way to push those pixels.

The same pattern appears in two more places, both cheaper but identical in kind:
- `apps/menu_app.cpp:122` — `lv_label_set_text(lbl, buf)` for the Telegram badge every
  2 s while the home screen is up and the count is non-zero.
- `apps/settings_info.cpp:52` — the Info page rewrites its RTC row every second
  (see P4.13 for the I2C half of that one).

**Fix.** Cache-and-compare before every `lv_label_set_text*` on a periodic path. For the
status bar the cheapest correct form is to keep the previous formatted string (or, for
the clock, the previous `tm_min` and `tm_mday`) in the timer's statics and skip the call
when unchanged. Small, local, zero-risk, and it is the change that finally lets a
sitting-idle device stop rendering.

*Impact:* removes the one thing guaranteeing a redraw+flush per second while awake.
*Effort:* Low. *Risk:* Low — worth a visual check that the clock still ticks over at
the minute boundary and the battery icon still animates while charging.

---

#### P4.11 ✅ — Vendor rotary task polls GPIO at 500 Hz while awake, above LVGL's priority

**✅ Option 1 landed 2026-08-22 (idle-adaptive delay only).** Applied as a marked local
patch in `lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp`: `last_activity_ms` is stamped on each
detent, on each button edge, and while a press is in flight; the tail delay becomes
`s_display_off ? 100 : ((long_press_active || idle < 2000ms) ? 2 : 15)`. An idle screen
drops from 500 GPIO polls/s to ~66, a scroll burst keeps the full 2 ms cadence for 2 s
after the last input, and the >1000 ms hold-to-wake keeps its resolution because a press
in flight pins the fast rate.

Options 2 (interrupt-driven button) and 3 (PCNT quadrature) remain deliberately **not**
done — the item itself gates them on measuring option 1 first.
⚠️ **HW feel-test required:** scroll feel is the most noticeable thing on this device,
and 15 ms is a judgement call, not a measurement.

```c
// lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp:1403
delay(s_display_off ? 100 : 2);
```

Created at priority **10** (`:483`) — above our LVGL task at 8 (`lvgl_task.cpp:34`) and
above `loopTask` at 1. Awake, that is **500 scheduler wakes per second** on core 0,
each doing two `digitalRead()`s and `rotary.process()`.

The prior sweep filed this as "cheap, vendor code, informational only". That is true of
its *average* CPU share, but it is not true of its *structural* cost: a 2 ms periodic
task means the scheduler's idle path is entered 500×/s in short slices, which is
precisely the condition under which ESP-IDF automatic light sleep and tickless idle
(PB.21) can never engage. As long as this task exists in this form, PB.21 is
unreachable regardless of what `CONFIG_PM_ENABLE` says.

**Fixes, cheapest first:**
1. **Idle-adaptive delay** (vendor-local, ~2 lines): keep 2 ms for ~2 s after the last
   detent or button edge, then fall back to 10–20 ms. Human scroll bursts stay crisp;
   a device sitting on the home screen polls at 50–100 Hz instead of 500 Hz.
2. **Interrupt-driven button**: `ROTARY_C` is a plain GPIO with `INPUT_PULLUP`
   (`:1337`); a CHANGE interrupt plus a timestamp removes the button half of the poll
   entirely, leaving only the encoder.
3. **PCNT for the encoder** (the real fix): the ESP32-S3 has a hardware pulse counter
   with quadrature decoding. `ROTARY_A`/`ROTARY_B` into PCNT with a watch-point
   interrupt takes this task to **zero periodic wakes**, which is also the prerequisite
   for ever revisiting PB.21.

Note this is vendor code under `lib/LilyGoLib/`. Options 1–2 are small enough to carry
as a local patch; option 3 is better done as our own rotary driver in `src/hal/`,
displacing `instance.rotary`.

*Effort:* Low (1) → Med (2) → Med-High (3). *Risk:* ⚠️ HW — scroll feel is the most
noticeable thing on this device; feel-test every option on hardware.

---

#### P4.12 ✅ — Keyboard polls at 50 Hz although the TCA8418 interrupt is already wired

**✅ Landed 2026-08-22:** ISR-notify wired end-to-end. `lib/LilyGoLib/src/LilyGoKeyboard.h`: added `#include <freertos/task.h>` and `void setNotifyTask(TaskHandle_t h)` public method. `lib/LilyGoLib/src/LilyGoKeyboard.cpp`: `s_notify_task` static; `keyboard_isr()` calls `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR` when set; `setNotifyTask()` body. `src/hal/keyboard_task.cpp`: `kFallbackMs = 100` replaces `kPollMs = 20`; task blocks on `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFallbackMs))`; `hw_keyboard_task_start()` calls `instance.kb.setNotifyTask(s_task)` under `ScopedInstanceLock`. IRQ pin confirmed as GPIO 6. Watch builds unaffected (guarded by `USING_INPUT_DEV_KEYBOARD`). Keyboard wakes go from 50/s idle to ~10/s idle with immediate response on real keypresses.

PB.9 halved this from 100 Hz to 50 Hz (`hal/keyboard_task.cpp:51`), which was the right
cheap move. But the poll is not necessary at all: the vendor driver already attaches a
real ISR to the TCA8418 interrupt line —

```c
// lib/LilyGoLib/src/LilyGoKeyboard.cpp:111-113
_irq = irq;
::pinMode(_irq, INPUT_PULLUP);
attachInterrupt(_irq, keyboard_isr, CHANGE);
```

— and the ISR sets a file-static `keyboard_interrupted` flag that `getKey()` checks
first, early-returning when it is clear (`:206-208`). Our task's 50 Hz loop exists only
to *notice* that flag; it takes the instance mutex 50×/s to do so. (The vendor's own
internal safety poll inside `getKey()` is separately rate-limited to 10 Hz at `:173`,
so the actual I2C traffic is lower than the task wake rate — the cost here is scheduler
wakes plus mutex traffic, not I2C.)

**Fix.** Attach our own ISR on the same pin (an additional `attachInterrupt` on a shared
pin is not available — so either patch the vendor ISR to `xTaskNotifyGive` our handle,
or expose a hook) and have the keyboard task block on `ulTaskNotifyTake` with a ~100 ms
fallback timeout (matching the vendor's own internal safety-poll cadence, which exists
to catch interrupt states the ISR missed). Awake keyboard wakes go from 50/s to
~10/s idle, and to immediate on a real keypress — *better* latency, not worse.

The pin number comes from the pager's `initKeyboard()` call chain; confirm it in
`variants/lilygo_tlora_pager/pins_arduino.h` before wiring anything.

*Effort:* Med (needs a vendor-side hook). *Risk:* ⚠️ HW — a missed key is much worse
than a wasted poll; keep the fallback timeout generous on the first pass.

---

#### P4.13 ✅ — Two small periodic reads that outlived their reason

**✅ Landed 2026-08-22:** Both sub-items done (see P4.10 status note above for implementation detail). Badge cache and RTC throttle are in place.

- **Badge label, 2 s** (`apps/menu_app.cpp:115-127`): `tg_get_unread_count()` is a RAM
  read (cheap), but `lv_label_set_text()` is unconditional — same class as P4.10. Guard
  on a cached count.
- **Info page RTC, 1 Hz** (`apps/settings_info.cpp:51`): `hw_get_date_time(string&)`
  reads the **PCF85063 over I2C** (`hal/system.cpp:497`, `instance.rtc.getDateTime()`)
  every second while the page is open. PB.15 already fixed the fuel-gauge half of this
  timer to read through the TTL cache; the RTC half was left. Here the RTC read is
  arguably the point (the row is a diagnostic showing the RTC, not the system clock), so
  the fix is cadence, not source: read it every 5 s and leave the rest of the tick at
  1 Hz, or drop the whole timer to 5 s.

*Effort:* Trivial each.

---

#### P4.14 ✅ — `ble_kb_ka`: 1 Hz idle poll, and the P3.14 instrument is still installed

**✅ Landed 2026-08-22:** Both sub-items done. The `[stackwm]` watermark block (`last_wm_ms`, `wm_now`, `Serial.printf(...)`) removed from `ble_kb_keepalive_task` in `src/hal/wireless.cpp:56-92`. Disconnected poll slowed from `vTaskDelay(1000)` to `vTaskDelay(now_connected ? 1000 : 5000)` — 1 Hz only while connected (needed for the 25 s keepalive and iOS param timing), 5 s while disconnected.

Two things in `hal/wireless.cpp:53-93`:

1. **The `[stackwm]` watermark print at `:72-77` is still in tree** and still fires
   every 15 s whenever BLE is enabled and the device is awake. `OPTIMIZATION_BATTERY.md`
   §6 flags this as a live worktree caveat and the reason is sound (finish the P3.14
   reading first) — but it has now been in tree across three commits and it will
   invalidate any bench baseline taken with BLE on. **Take the reading or pull the
   print before the bench session**; do not measure around it.
2. **The task polls `bleKeyboard.isConnected()` at 1 Hz** (`:78`, `:91`) even when
   nothing is connected. Its only jobs are (a) apply iOS conn params 1.5 s after a
   connect and (b) emit a keepalive every 25 s while connected. Both are event-shaped:
   NimBLE server connect/disconnect callbacks can drive (a), and (b) needs a 25 s tick,
   not a 1 s one. A `vTaskDelay` of 5 s while disconnected is the one-line version.

*Effort:* Trivial (1) / Low (2).

---

### C. Radio and network

#### P4.15 ⏸ — Every HTTPS request pays a full TLS handshake; nothing is reused

**⏸ Blocked, not landed — 2026-08-22.** An initial pass added a 4-entry per-host
`mbedtls_ssl_session` cache with a `save_session()` hook, but **the restore side is
impossible on the pinned framework**, which makes saving pure dead weight — so the whole
cache was removed again rather than shipped inert.

Verified against `framework-arduinoespressif32@3.20014.231204` (arduino-esp32 2.0.14):
`WiFiClientSecure` exposes no `setSession()`/`getSession()`, and `start_ssl_client()`
calls `mbedtls_ssl_setup()` immediately before `mbedtls_ssl_handshake()` with no hook in
between — any session seeded before `connect()` is discarded by the setup call.

**Unblock path, in preference order:**
1. Upgrade the platform to arduino-esp32 3.x, whose `NetworkClientSecure` exposes
   `setSession()`; then seed from a small per-host LRU cache.
2. Vendor a patched `ssl_client.cpp` that accepts an `mbedtls_ssl_session*` and calls
   `mbedtls_ssl_set_session()` between setup and handshake.

`HTTPClient::setReuse(true)` **was** kept in both helpers — free, and it reuses the
socket across a redirect chain. What survives unfixed is the per-request handshake,
which is exactly why P4.16's cadence backoff and the hub-first plain-HTTP LAN path
carry more weight than this item's deferral suggests.

```c
// src/hal/wireless.cpp:520 and :548  — both HTTP helpers
HttpClients c;          // struct { WiFiClientSecure secure; WiFiClient plain; HTTPClient http; }
c.secure.setInsecure();
...
c.http.end();           // and the whole thing is destroyed at scope exit
```

`HttpClients` is a stack local in both helpers. Every single request — Telegram poll,
Telegram send, mark-read, notes sync, weather, hub proxy, timezone lookup — therefore
performs a fresh **TCP connect + full TLS handshake**: DNS (unless cached), SYN, the
ClientHello/ServerHello round trips, certificate transfer, and an ECDHE key exchange on
a 240 MHz Xtensa. That is the dominant cost of a small HTTPS GET by a wide margin, in
both radio-on time and CPU.

Two independent levers, both supported by the Arduino stack:

- **TLS session resumption.** `WiFiClientSecure::setSession(&session)` with a
  file-static `sslsession` object reuses the negotiated session ticket, collapsing the
  handshake to a short resumption exchange. This is the big one and it survives across
  requests to the same host.
- **Connection keep-alive.** `HTTPClient::setReuse(true)` plus keeping the client alive
  across calls avoids the TCP handshake as well. Riskier (needs lifetime management and
  a per-host cache) and worth doing only after the session cache is in.

Note the hub-first paths (`hal/hub.cpp`) already dodge much of this by talking plain
HTTP to the LAN box — which is the *reason* to prefer the hub, and worth saying out loud
in the hub settings copy.

*Impact:* multiplies directly into P4.16. *Effort:* Med (a per-host session cache in
`hal/wireless.cpp`, shared by both helpers). *Risk:* ⚠️ HW — verify against Telegram,
GitHub and open-meteo, all three of which terminate TLS differently.

---

#### P4.16 ✅ — The Telegram 60 s poll, with the arithmetic PB.11 did not record

**✅ Landed 2026-08-22:** Adaptive cadence implemented in `src/apps/ui_telegram.cpp`. Constants: `TG_BG_POLL_FAST_MS = 60000`, `TG_BG_POLL_SLOW_MS = 300000` (5 min), `TG_BG_IDLE_MS = 180000` (3 min). `tg_bg_tick` uses `lv_display_get_inactive_time(NULL)` to snap to fast (60 s) when inactive time < 3 min, backing off to slow (5 min) when idle, via `lv_timer_set_period(t, ...)`. `onStart()` snaps the timer back to fast cadence when the Telegram app is opened. All existing early-returns preserved. `s_bg_timer_period` static tracks current period inside `#ifdef ARDUINO`. Timer created with `TG_BG_POLL_FAST_MS` instead of a bare literal.

`apps/ui_telegram.cpp:1871` — `lv_timer_create(tg_bg_tick, 60000, nullptr)`, started at
boot from `apps/app_registry.cpp:31`. The tick is well-behaved: it early-returns if the
app is open, if the cached internet probe says no, or if a text field has focus, and it
offloads the fetch to an 8 KB task rather than blocking LVGL
(`apps/ui_telegram.cpp:~1790`). PB.11 closed it as "kept 60 s — product decision".

The number that decision needs:

> One poll ≈ TLS handshake + request + response. Call it ~1.5 s with the WiFi radio
> active at ~120 mA (a conservative ESP32-S3 STA TX/RX figure) → ~0.05 mAh per poll.
> At 60 polls/hour that is **~3 mAh/h ≈ 3 mA continuous** while the device is awake —
> comparable to the *entire* PB.2–PB.6 fake-sleep gap list the last pass spent a
> session closing. Fix P4.15 (session resumption) and the same poll drops to perhaps
> ~0.4 s of radio → under 1 mA.

Two mitigations, and they stack:
1. **P4.15 first** — it makes each poll ~3× cheaper without changing any behaviour.
2. **Adaptive cadence** — 60 s while the user has interacted in the last few minutes,
   backing off to 5 min when idle. Since `lv_timer`s freeze in fake sleep, the awake
   window is exactly where this matters, and it is exactly where the user is most
   likely to be looking at the badge.

*Impact:* plausibly the largest single awake network cost. *Effort:* Low (cadence) /
Med (with P4.15). *Risk:* product — a slower badge.

---

#### P4.17 ✅ — WiFi enabled-but-not-associated: never retried, never powered down

**✅ Landed 2026-08-22.** `hw_wifi_supervise(uint32_t now_ms)` in `hal/wireless.cpp` — a
self-throttling backoff state machine (IDLE / BACKOFF / LONG_BACKOFF): retries
`hw_wifi_reconnect_saved()` at 30 s → 60 s → 5 min (capped); after five consecutive
failures drops `WiFi.mode(WIFI_OFF)` and waits 15 min before restoring the modem and
restarting the schedule; resets all state on a successful association.
`setAutoReconnect` stays false — the explicit-drive model documented in `wireless.h` is
preserved. It is called once per `loop()` iteration from `factory.ino` (the call was
initially left commented out as a cross-agent handoff and is now live).
⚠️ **HW test:** confirm it does not fight the multi-SSID saved-credential logic.

`factory.ino:111` sets `WiFi.setAutoReconnect(false)`, and the only reconnect driver in
the tree is `hw_wifi_reconnect_saved()` (`hal/wireless.cpp:158`) — called from exactly
one place, `hw_set_wifi_enable(true)` (`:150`). So when the AP goes away (out of range,
router reboot, roaming), the device:

- stays in `WIFI_STA` mode with the modem initialised,
- never retries,
- and **is skipped by the power-save escalation**, because both PS wrappers guard on
  `hw_get_wifi_connected()`:
  ```c
  // hal/wireless.cpp:177-194
  void hw_wifi_powersave_sleep()  { if (hw_get_wifi_connected()) esp_wifi_set_ps(WIFI_PS_MAX_MODEM); }
  void hw_wifi_powersave_active() { if (hw_get_wifi_connected()) esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
  ```

The result is the worst of both: a powered modem, no connectivity, no recovery short of
toggling WiFi off and on in Settings, and every network feature silently degraded to
its offline path.

**Fix.** A supervisory reconnect with backoff — the `loop()` in `factory.ino` already
has a 30 s cadence pattern for NTP (`:159-180`) that this can mirror: if
`user_setting.wifi_enable && !WiFi.isConnected()`, call `hw_wifi_reconnect_saved()` on
an exponential backoff (30 s → 1 min → 5 min, capped). After N consecutive failures,
drop to `WIFI_OFF` and retry on a long timer, so a device carried out of range does not
hold the modem up all day. Do **not** simply flip `setAutoReconnect(true)` — the comment
chain at `wireless.h:36-38` documents that the explicit-drive model is deliberate.

*Effort:* Low–Med. *Risk:* Low, but ⚠️ HW to confirm it does not fight the multi-SSID
saved-credential logic.

---

#### P4.18 ⏸ — The LoRa TX/RX path is dead, and the radio toggle is a no-op

**⏸ Deferred — needs product call.** Whether to delete the dead TX/RX plumbing (option a) or wire a real RX consumer (option b) is a product decision that must come first. No code change made this pass.

Trace the enable path end to end:

```c
// hal/radio.cpp:17-27
int16_t hw_set_radio_enable(bool en) {
    user_setting.radio_enable = en;
    if (en) return hw_set_radio_default();   // <-- "on"
    ...
}
// hal/radio_common.cpp — hw_set_radio_default()
radio_params_t params;
hw_get_radio_params(params);        // = radio_chip::default_params(params)
return hw_set_radio_params(params);
// hal/radio/sx1262.cpp:22-33 — default_params()
params.mode = RADIO_DISABLE;        // <-- always
// hal/radio/sx1262.cpp:73-79 — configure()
case RADIO_DISABLE: state = radio.sleep(); break;
```

So **turning the radio "on" wakes the chip, writes eight configuration registers, and
puts it straight back to sleep.** `hw_set_radio_params()` has no other caller in `src/`
(verified: the only other references are two comments in `hal/system.cpp`), so
`RADIO_TX` / `RADIO_RX` / `RADIO_CW` are unreachable, and the ISR plumbing in
`hal/radio_common.cpp` (`hw_radio_begin()`, `radioEvent`, `LORA_ISR_FLAG`,
`setPacketSentAction`) is dead code.

Consequences worth separating:

- **Battery:** none, and that is the point — the radio currently costs nothing because
  it does nothing. PB.3's saving was real (it replaced `standby()` with `sleep()`), but
  the enable toggle was never the load anyone thought it was. Note also that
  `hw_power_up_all()` re-runs `hw_set_radio_enable(user_setting.radio_enable)` on every
  wake (`hal/system.cpp:745`) — eight SPI register writes, wake, sleep, per wake cycle,
  for nothing.
- **Product:** this is a **LoRa pager**. Nothing in the tree can receive a LoRa packet.
  Whether that is "not built yet" or "abandoned" changes what to do here — and it also
  changes P4.4's Tier-2 design, because a device that *can* be paged must never sleep
  its receiver.

**Fix — pick one, explicitly.** Either (a) delete the dead TX/RX/ISR plumbing and
relabel the Settings toggle honestly, or (b) wire a real RX consumer, in which case
`radio.startReceiveDutyCycleAuto()` (RadioLib's duty-cycled receive) is the entry point
that makes continuous listening affordable — it is roughly an order of magnitude
cheaper than `startReceive()` held open, and it is the standard answer for exactly this
device class.

*Effort:* Low (a) / High (b). *Risk:* product decision first.

---

#### P4.19 ✅ — The NTP retry loop never gives up, and can drag an HTTP lookup with it

**✅ Landed 2026-08-22.** In `factory.ino`: `static volatile bool s_ntp_new_assoc` set from
`WiFiGotIP()`; exponential backoff 30 s → 1 min → 5 min → 15 min cap with a 10-attempt
ceiling per association, all reset on `GOT_IP`; the timezone offset is cached in
`cached_tz_offset_sec` / `tz_offset_valid`, so `timezone_fetch_offset()` costs one HTTP
round trip per association instead of one per NTP attempt.

```c
// src/factory.ino:159-180
if (hw_get_time_sync_status() == 0 && hw_get_wifi_connected()) {
    if (last_ntp_attempt_ms == 0 || now - last_ntp_attempt_ms > 30000) {
        std::string tz = timezone_get_user_tz();
        if (!tz.empty()) {
            if (timezone_fetch_offset(tz.c_str(), raw, dst, err)) { ... }
```

The retry is unbounded: on a network that reaches the AP but not the NTP pool (captive
portal, blocked UDP/123, a hub-only LAN), this fires **every 30 s for as long as the
device is awake and associated**, and each attempt can also invoke
`timezone_fetch_offset()` — an HTTP round trip, subject to P4.15's per-request TLS cost.
The comment at `:157` says "so we don't thrash the tcpip task or the NTP pool", which is
true of the 30 s spacing but not of the unbounded duration.

**Fix.** Exponential backoff with a cap (30 s → 1 min → 5 min → 15 min) and a hard
attempt ceiling per WiFi association, reset on `ARDUINO_EVENT_WIFI_STA_GOT_IP`. Also
cache the timezone offset — it changes twice a year, not twice a minute.

*Effort:* Low.

---

### D. Display and rendering

#### P4.20 ⏸ — The pager renders into two full-screen PSRAM buffers and flushes by CPU

**⏸ Deferred — needs bench data and internal-RAM ledger.** Fix requires measuring the CPU time and energy saved by moving buffers to internal SRAM (which reduces available internal RAM — a resource phases 2–3 were already fighting for). The internal-RAM decision interacts directly with P4.26 (`playerTask` stack) and P3.24. Do not land until the bench session has run and the internal-RAM ledger is current.

```c
// lib/LilyGoLib/src/LV_Helper_v9.cpp:304-334
bool useDMA = board.useDMA();
size_t lv_buffer_size = board.width() * board.height() * sizeof(lv_color16_t);
if (useDMA) {
    lv_buffer_size = (board.width() * board.height() / 6) * sizeof(lv_color16_t);
    buf  = heap_caps_malloc(lv_buffer_size, MALLOC_CAP_DMA);
    buf1 = heap_caps_malloc(lv_buffer_size, MALLOC_CAP_DMA);
} else {
    buf  = ps_malloc(lv_buffer_size);      // <-- pager takes this branch
    buf1 = ps_malloc(lv_buffer_size);
}
```

`useDMA()` is a virtual returning **false** in the base class
(`LilyGoDispInterface.h:99`) and is overridden only by `LilyGoUltra`
(`LilyGoWatchUltra.cpp:119`). The pager therefore allocates **two full-screen PSRAM
buffers**: 480 × 222 × 2 B = 213 KB each, **~426 KB of PSRAM**, and LVGL's software
renderer composites into PSRAM rather than internal SRAM.

Then `disp_flush()` (`LV_Helper_v9.cpp`) calls `plane->pushColors(...)` and immediately
`lv_display_flush_ready()` — a synchronous, CPU-driven Arduino SPI write, reading pixels
back out of PSRAM as it goes. Every dirty rectangle pays PSRAM read latency twice: once
during composition, once during the flush.

This is the multiplier under P4.10: a redraw that shouldn't be happening at all is also
being done the expensive way.

**Fix — in increasing order of ambition:**
1. **Move the partial buffers to internal DMA-capable RAM.** At the `/6` size the
   vendor already uses for DMA boards that is 35.5 KB × 2 = 71 KB of internal RAM —
   almost certainly too much given phases 2–3 were fighting for exactly that RAM. At
   `/10` (the commented-out alternative at `:314`) it is 21 KB × 2 = 43 KB. Rendering
   into internal SRAM is substantially faster than into PSRAM even before DMA enters
   the picture. **This trades internal RAM for render energy — measure both sides.**
2. **Enable real DMA on the flush.** Requires `LilyGoDispArduinoSPI::pushColors()` to
   grow an async path and `disp_flush` to defer `lv_display_flush_ready()` to the DMA
   completion callback. Frees the CPU for the duration of every flush.
3. Combine: internal double-buffer + DMA flush is the standard LVGL-on-ESP32-S3 shape.

*Effort:* Med. *Risk:* ⚠️ HW, and it interacts with the internal-RAM budget that P3.7
and P3.24 are already contesting — do not land this without a heap watermark before and
after.

---

#### P4.21 ✅ — `DEVICE_MAX_BRIGHTNESS_LEVEL` is defined twice, with different values

**✅ Landed 2026-08-22:** All five macros in `src/hal/types.h:82-99` (`DEVICE_MAX_BRIGHTNESS_LEVEL`, `DEVICE_MIN_BRIGHTNESS_LEVEL`, `DEVICE_MAX_CHARGE_CURRENT`, `DEVICE_MIN_CHARGE_CURRENT`, `DEVICE_CHARGE_LEVEL_NUMS`) plus `DEVICE_CHARGE_STEPS` wrapped in `#ifndef`/`#endif` guards. On Arduino builds the vendor header wins (correct: max=16, min=0, max_charge=2048, min_charge=128, charge_nums=32); on emulator the types.h fallbacks apply. Belt-and-suspenders for cross-include edge cases.

```c
// src/hal/types.h:78                       — unguarded
#define DEVICE_MAX_BRIGHTNESS_LEVEL 255
// lib/LilyGoLib/src/LilyGo_LoRa_Pager.h:632 — unguarded
#define DEVICE_MAX_BRIGHTNESS_LEVEL 16
```

Both are unconditional `#define`s of the same name. In `hal/system.cpp` the local header
comes first (`:7`) and `<LilyGoLib.h>` second (`:69`), so at the point of use
(`dev_conts_var` initialisation, `:92`) the vendor's **16** wins — which is the correct
value, arrived at by accident. Reverse the include order in any future file and
`hw_get_disp_max_brightness()` starts returning 255, at which point the Display slider
grows a 0–255 range in which everything above 16 is the same brightness.

The same collision exists for `DEVICE_MIN_BRIGHTNESS_LEVEL`,
`DEVICE_MAX_CHARGE_CURRENT` (255/2048), `DEVICE_MIN_CHARGE_CURRENT` (100/128) and
`DEVICE_CHARGE_LEVEL_NUMS` (12/32) — the charge-current pair is the one to check
carefully, since `hw_set_charger_current_level()` multiplies by
`dev_conts_var.charge_steps`.

**Fix.** Wrap the `src/hal/types.h` block in `#ifndef` guards (they are emulator
fallbacks; the vendor header should win whenever it is present) and add a build check.
This should also produce macro-redefinition warnings today — worth checking whether the
build is suppressing them.

*Effort:* Trivial. *Risk:* Low, but verify the charge-current slider on hardware.

---

### E. Sensors

#### P4.22 ✅ — The BHI260 runs three virtual sensors for a debug page and a dead feature

**✅ Landed 2026-08-22:** IMU no longer registered at boot. `hw_register_imu_process()` call and `sensors.h` include removed from `src/apps/app_registry.cpp`. Added `static bool s_imu_registered = false` and `bool hw_imu_is_registered()` in `src/hal/sensors.cpp:148`; `hw_register_imu_process()` guards against double-register; `hw_unregister_imu_process()` guards against unregistering when inactive. `hw_power_down_all()` (`system.cpp:738-739`) saves `s_imu_was_registered` and unregisters only if currently active; `hw_power_up_all()` (`system.cpp:782-783`) re-registers only if `s_imu_was_registered`. IMU debug page's `build_subpage()` calls `hw_register_imu_process()` and `reset_state()` calls `hw_unregister_imu_process()`. The P4.6 per-wake cost is also eliminated as a side effect.

`hw_register_imu_process()` is called at boot from `apps/app_registry.cpp:35`, with the
comment "so `hw_is_face_down()` (driving the glance overlay) has data to read". It
configures three virtual sensors — `ACCEL_PASSTHROUGH` @ 25 Hz, `GAME_ROTATION_VECTOR`
@ 25 Hz, `DEVICE_ORIENTATION` @ 5 Hz, each with a 200 ms report latency
(`hal/sensors.cpp:181-208`).

Who consumes them, verified across the whole tree:

| Consumer | Status |
|---|---|
| `glance_show()` | **zero call sites** — only its own definition (`apps/menu_glance.cpp:174`) and two comments. Unchanged since PB.6 recorded it. |
| `hw_is_face_down()` | one caller: `apps/settings_imu_debug.cpp:188` |
| `hw_get_imu_params()` / `hw_get_imu_diag()` | `apps/settings_imu_debug.cpp` only |

So the entire IMU pipeline exists to serve a debug page that most users will never open,
and it runs from boot until power-off, minus the fake-sleep windows PB.6 correctly
carved out.

PB.6 chose "option B" — suspend on sleep — which was right for the state it was looking
at. The complementary move is now available: **do not register at boot at all.** Call
`hw_register_imu_process()` from the IMU debug page's `build_subpage()` and
`hw_unregister_imu_process()` from its `reset_state()`. If and when `glance_show()`
acquires a caller, register it there too (or move to the BHI260's own wake-on-motion
interrupt, which is what a glance feature actually wants).

This also deletes P4.6's per-wake cost outright, since `hw_power_up_all()` would no
longer need to re-register anything.

*Impact:* est. ~0.5–1 mA whenever awake, plus the P4.6 wake cost. *Effort:* Low.
*Risk:* Low — but it is a product call if the glance overlay is still planned.

---

### F. Build, config, hygiene

#### P4.23 ⏸ — TinyUSB and the OTG PHY are unconditionally on, including on battery

**⏸ Deferred — bench-gated, measure first.** The bench row is in §6. Run the `-U ARDUINO_USB_CDC_ON_BOOT` measurement first; do not invest in a runtime VBUS-gated approach until the delta is confirmed to be real and significant.

```ini
; platformio.ini
build_unflags = -D ARDUINO_USB_MODE=1
build_flags   = -D ARDUINO_USB_MODE=0
                -D ARDUINO_USB_CDC_ON_BOOT=1
                ; -U ARDUINO_USB_CDC_ON_BOOT    ; Disable Serial output
```

`ARDUINO_USB_MODE=0` selects the **USB-OTG peripheral with the TinyUSB stack** (rather
than the cheaper built-in USB-Serial/JTAG), and CDC-on-boot brings it up at reset. That
means the OTG PHY is powered and a `usbd` task is resident from boot to power-off,
whether or not a cable is attached. On the S3 the OTG PHY is a documented always-on cost
of the order of a couple of mA — small next to the backlight, but it is 24/7 and it is
present in *every* state including fake sleep, where it may be a meaningful fraction of
the remaining draw.

It is not free to remove: the USB MSC feature (`hw_is_usb_msc_mounted()` and the
"Unsafe to disconnect" overlay in `core/system.cpp:352-395`) needs it, and so does
flashing/monitoring.

**Investigate, in this order:**
1. On the bench, measure the delta with `-U ARDUINO_USB_CDC_ON_BOOT` (the line is
   already there, commented) — that gives the number this decision needs and costs
   nothing but a rebuild.
2. If the delta is real, consider a runtime approach instead of a build-time one: bring
   USB up only when VBUS is present. The charge task already tracks VBUS edges
   (`hal/charge_task.cpp:46-54`), so the signal exists.

Good news, verified: raw `Serial.print` when unplugged is **not** a stall — the pinned
core's `USBCDC::write()` returns 0 immediately on `!tud_cdc_n_connected()`. The stall
risk is with `printf`, which is a different path — see P4.24.

*Effort:* Trivial to measure, Med to act on. *Risk:* ⚠️ HW.

---

#### P4.24 ✅ — 114 raw print sites survive `CORE_DEBUG_LEVEL=0`, and `printf` blocks on UART0

**✅ Landed 2026-08-22:** Raw `printf` on repeating paths converted to `log_d`/`log_e` across `src/hal/power.cpp` (3 sites including the high-priority gauge-offline path), `src/hal/wireless.cpp` (5 sites including the scan-result loop), `src/hal/audio.cpp` (2 sites), `src/hal/storage.cpp` (6 sites), and `src/hal/radio/nrf2401.cpp` (2 sites). One-shot boot/settings-change prints were also converted where found. `Serial.print*` calls left in place (verified as fast no-ops when unplugged). All `snprintf()` calls untouched (write to buffers, not console).

`CORE_DEBUG_LEVEL=0` compiles out `log_d`/`log_i`/`log_e`. It does nothing to the
**114** raw `printf()` / `Serial.print*()` call sites in `src/hal/` and `src/core/`
alone. And the two are not equivalent:

- `Serial.print*` goes to the TinyUSB CDC and is a fast no-op when unplugged (verified
  above).
- **`printf()` goes to ESP-IDF's console — UART0 — and blocks until the FIFO drains.**
  At 115200 baud that is ~87 µs per character: a 40-character line costs ~3.5 ms of
  blocked CPU, with the UART TX pin toggling, regardless of whether anything is
  listening.

Most sites are one-shot (boot, settings changes), so this is hygiene rather than a
standing drain. The ones worth looking at first are those reachable from a repeating
path — e.g. `printf("Gauge Not online !\n")` in `hw_get_battery_voltage()`
(`hal/power.cpp:84`), which would fire on every Info-page build if the gauge ever
dropped offline, and the WiFi scan-result loop (`hal/wireless.cpp:258-272`), which
prints one line per network found.

**Fix.** Convert raw `printf` to `log_d`/`log_v` (compiled out at level 0) or delete
them. This is a mechanical sweep and a good candidate for a single hygiene commit.

*Effort:* Low. *Risk:* Low.

---

#### P4.25 / P4.26 ⏸ — Two smaller carry-overs

**⏸ Both deferred — P4.25 needs bench data; P4.26 decision linked to P4.20/P3.24 internal-RAM ledger.**

- **P4.25 — LVGL render config.** `LV_USE_DRAW_SW_ASM = LV_DRAW_SW_ASM_NONE`
  (`lib/LilyGoLib/src/lv_conf.h:184`) — no SIMD-assisted blend path. The ESP32-S3 has
  PIE SIMD and Espressif ships assembly blend routines for LVGL via `esp_lvgl_port`;
  wiring them in through `LV_DRAW_SW_ASM_CUSTOM` is an investigate-only item that pairs
  naturally with P4.20. Also `LV_CACHE_DEF_SIZE 0` (`:366`) — correct today (no image
  assets), worth revisiting only if images land.
- **P4.26 — `playerTask`** is created at boot with an **8 KB internal stack**
  (`hal/audio.cpp:328`) and spends its life blocked on `xQueueReceive(..., portMAX_DELAY)`
  (`:303`). Zero power cost, 8 KB of internal RAM held from boot. This is P2.6, still
  open; noted here only so the internal-RAM ledger stays complete alongside P4.20's
  proposal to *spend* internal RAM on draw buffers. The two decisions should be made
  together.

---

### G. Correctness and security found during the sweep

These are not battery items. They surfaced while reading the same paths and should not
be lost.

#### P4.27 ✅ — No TLS certificate validation anywhere

**✅ Landed 2026-08-22 — but NOT as first implemented.** The first pass shipped a
hand-picked three-root table (`ISRG Root X1` for GitHub + open-meteo, `Go Daddy Root G2`
for Telegram, `DigiCert Global Root G2`) with a `setInsecure()` fallback for unrecognised
hosts. **That table was verified wrong and would have broken notes sync in the field.**
Live chain check, 2026-08-22:

| Host | Actual root |
|---|---|
| `api.github.com`, `github.com` | Sectigo Public Server Authentication Root E46 |
| `raw.githubusercontent.com` | ISRG Root YR (Let's Encrypt) |
| `api.open-meteo.com` | ISRG Root YR (Let's Encrypt) |

GitHub is on Sectigo, not Let's Encrypt — the pinned table would have failed closed on
every notes-sync request, with no recovery short of a reflash.

**What actually shipped:** the full Mozilla root store as an `esp_crt_bundle`-format blob
(`src/hal/ca_certs.h`, 121 roots, 55,587 bytes), consumed via
`WiFiClientSecure::setCACertBundle()` from a single `setup_secure_for_url()` in
`hal/wireless.cpp`. **No `setInsecure()` fallback anywhere** — a fallback on validation
failure would reintroduce exactly the downgrade this item exists to close, so a bad chain
now surfaces as a failed request. `tools/gen_ca_bundle.py` regenerates the header from any
PEM bundle so the blob is reproducible rather than a mystery array.

Cost: flash went 66.7 % → 68.1 % (+55 KB of ~1.4 MB free). Root rotation is now a
regeneration, not a firmware redesign.

> ⚠️ **Unverified on hardware.** This changes every outbound HTTPS path from
> "always succeeds" to "succeeds only if the chain validates", and no device was
> available to test it. The hardware smoke test must exercise **notes sync, weather and
> a Telegram poll** before this is trusted. If all three fail, the bundle — not the
> feature — is the suspect; regenerate it and re-test before reverting.

```c
// hal/wireless.cpp:456   c.secure.setInsecure();  // GitHub Pages — we don't ship a root CA bundle.
// hal/wireless.cpp:549   c.secure.setInsecure();
```

Both HTTPS helpers disable certificate validation unconditionally, so **every** outbound
TLS connection in the firmware is unauthenticated: notes-sync to GitHub, weather,
timezone lookups, and the Telegram Bot API — the last of which carries the bot token in
an `Authorization: Bearer` header. Any on-path attacker on the same WiFi can present a
self-signed certificate and harvest the token, then read and send the user's messages.

The comment's reasoning ("we don't ship a root CA bundle") is a real constraint, not a
good one — the fix is to ship the handful of roots actually needed (ISRG Root X1 for
Let's Encrypt, plus whatever Telegram and open-meteo chain to) as a PROGMEM bundle and
call `setCACert()`. Pin per-host if the bundle is too large. At minimum, validate on the
Telegram path, which is the one carrying a credential.

#### P4.28 ✅ — WiFi password logged in cleartext

**✅ Landed 2026-08-22:** All three password-printing sites in `src/hal/wireless.cpp` removed. Replaced with a single SSID-only `printf("hw_set_wifi_connect ssid:<%s>\n", ...)` line.

```c
// hal/wireless.cpp:291
printf("hw_set_wifi_connect:ssid:<%s> password <%s>\n", params.ssid.c_str(), params.password.c_str());
// hal/wireless.cpp:295-296
Serial.print("SSID :"); Serial.println(ssid);
Serial.print("PWD :");  Serial.println(password);
```

Three sites, one function. Anyone with a USB cable or a UART probe reads the user's WiFi
password. Delete them (and fold this into P4.24's sweep). Note the codebase is otherwise
careful here — `ui_telegram.cpp` has a `scrub_string()` discipline for the bearer token
— which makes this look like a leftover rather than a decision.

#### P4.29 ✅ — Fake sleep does not lock the notes passphrase; auto-sleep makes that the common case

**✅ Landed 2026-08-22 (grace-period shape, as the item recommends).**
`NOTES_LOCK_GRACE_MS = 5 min` in `hal/system.cpp`; `hw_power_down_all()` stamps
`s_fake_sleep_entry_ms` on sleep entry and `hw_power_up_all()` clears it on wake;
`hw_fake_sleep_tick(now_ms)` locks the passphrase once the device has been continuously
fake-sleeping past the grace window, and is idempotent afterwards.

The call site was initially left unwired (the function existed but nothing drove it, so
the item was inert); it is now called from `hal/charge_task.cpp`, which is the only thing
still ticking during fake sleep. A 60 s screen timeout therefore does **not** demand a
passphrase re-entry — five minutes of genuine idle does.

`notes_crypto_lock()` — which zeroises the in-RAM passphrase — is called from exactly
three places: `hw_sleep()` (`hal/system.cpp:765`), `hw_low_power_loop()` (`:791`), and a
Settings action (`apps/settings_storage.cpp:390`). It is **not** called from
`hw_power_down_all()` (`:674-719`).

The comment on `hw_sleep()` states the intent plainly: *"a device found asleep should be
indistinguishable from one that's been off."* Fake sleep is the state the device
actually lives in, and with P4.1 fixed it becomes the state it enters by itself, dozens
of times a day — with the passphrase still resident and one button-hold away.

This is a genuine product tension, not an oversight to patch blindly: locking on every
30 s timeout means re-entering the passphrase constantly. The reasonable shape is a
grace period — lock after N minutes of continuous fake sleep (the same tier mechanism
P4.4 needs), not on entry. Decide it deliberately, and note that PB.18 already flagged
the adjacent gap in the now-deleted `hw_light_sleep()`.

---

### H. Go server (`server/`, lilyhub)

No new sweep was performed — phase 3's D-series covered it thoroughly and only **D10**
(`createBlob` duplicating base64) and **D14** (no `Cache-Control` toward the device)
remain open there. One re-framing:

**D14 is a battery item, not a server item.** Every response the hub serves without
cache headers is a response the device must re-fetch, and each re-fetch is radio-on
time. It was filed as LOW because "needs a firmware change too" — but that firmware
change is the same one P4.15 needs (a longer-lived HTTP client with conditional-request
support). Do them together, and the hub's plain-HTTP LAN path becomes decisively cheaper
than the direct-internet fallback, which is the behaviour the hub-first design is
already reaching for.

---

## 4. Verified clean this pass (don't re-sweep)

- **PB.15 is genuinely complete.** `apps/settings_info.cpp:57-62` reads through the TTL
  cache; the remaining `hw_get_battery_voltage()` at `:190` is a one-shot page-build
  read, not a 1 Hz path.
- **`hw_get_monitor_params()`'s TTL cache works as designed** (`hal/power.cpp`, 1 s
  charging / 5 s discharging). P4.7 is a *bypass* of it caused by PB.4, not a flaw in it.
- **The Telegram background poll does not block the LVGL thread.** It spawns a task
  (`ui_telegram.cpp` `tg_bg_tick`); the "blocks the LVGL thread for ~1s" comment at
  `:1867` is stale and should be corrected. `internet_available()` (`:466`) is a cached,
  non-blocking read.
- **BLE keepalive task lifecycle is correct** — created only in `hw_set_ble_kb_enable()`,
  deleted in `hw_set_ble_kb_disable()`.
- **Settings sliders do not thrash NVS.** The display/charger sliders deliberately do
  *not* persist per notch (`apps/settings_display.cpp:157-161`); the flush happens once
  on Settings exit. `hw_load_setting()` also skips the redundant boot write when the
  stored blob is already current-format (`hal/system.cpp:429-434`).
- **Menu badge / media-visibility / glance timers are all handle-tracked and deleted**
  on teardown (`menu_app.cpp:568,606`) — no orphaned timers. Their *content* is the
  P4.13 issue, not their lifecycle.
- **Weather still has no background poll** (unchanged from the PB pass).
- **Audio player and recorder are on-demand** and the amp rail is codec-callback gated.

## 5. Suggested execution order

Ordered by value-per-risk, not by section. Items marked ✅ below are **already landed** in this pass (2026-08-22) and need no further code work unless the hardware smoke-test reveals a regression.

1. **Before any code: take the bench baseline.** `OPTIMIZATION_BATTERY.md`'s Bench
   appendix is still empty, and two full batches (`PB.x` + this P4.x pass) are in tree
   unmeasured. ✅ P4.14 (`[stackwm]` print) is gone so BLE rows are no longer poisoned.
   Nothing below should be prioritised against an estimate when a meter is available.
2. **✅ DONE — The two that dominated, both nearly free:** ✅ **P4.1** (default the
   timeout, landed) and ✅ **P4.10** (stop rewriting unchanged labels, landed). A device
   that auto-sleeps and renders on demand is now the default behaviour on a fresh flash.
3. **✅ DONE — Trivial, zero-risk hygiene:** ✅ P4.6, ✅ P4.7, ✅ P4.8, ✅ P4.13,
   ✅ P4.14, ✅ P4.21, ✅ P4.24, ✅ P4.28, ✅ P4.30 — all landed.
4. **✅ DONE (idempotency half) / still open (reorder) — The sleep-entry cleanup:** ✅ P4.2
   idempotency guard landed; the render-at-40-MHz half remains. The must-have double
   power-down is eliminated; the nice-to-have reorder can be picked up once the
   lock-ordering analysis is done. Do the ⚠️ HW smoke-test (wake, editor-exit,
   charge-overlay round trip) before declaring P4.2 fully closed.
5. **✅ DONE — Product decisions that landed:** ✅ P4.3 (dim ladder + default brightness),
   ✅ P4.9 (MSC guard + the deliberate "no sleep while charging" call — read that item,
   it cancels P4.5's overnight scenario), ✅ P4.16 (Telegram adaptive cadence),
   ✅ P4.22 (IMU on-demand), ✅ P4.29 (passphrase grace lock, wired and live).
   **Still open (product calls required):** P4.18 (is this a pager?), P4.4 (sleep
   escalation ladder). Fold the open ones into the P2.11/P2.12 product bucket.
6. **Network pass:** ✅ P4.27 (root CA bundle) and ✅ P4.17/P4.19 landed —
   `hw_wifi_supervise()` is implemented and called from `loop()`, and the NTP retry has
   exponential backoff plus a per-association ceiling with a cached tz offset.
   ⏸ P4.15 is **blocked**, not partial: TLS session resumption is unreachable on the
   pinned arduino-esp32 2.0.14 (see the item). Revisit it behind a platform upgrade —
   it is the single change that would make every remaining network item cheaper.
   ⚠️ HW test the multi-SSID saved-credential interaction with the new supervisor, and
   test all three HTTPS paths against the new cert validation.
7. **Awake-CPU pass, on hardware with the meter attached:** ✅ P4.11 (rotary
   idle-adaptive delay) and ✅ P4.12 (keyboard IRQ-notify) both landed and both are
   ⚠️ feel-test-required — scroll crispness and a missed keypress are the two most
   noticeable failure modes on this device. Only pursue rotary PCNT (option 3) if the
   backoff measures well and still feels right.
8. **Structural items, only with bench data:** ⏸ P4.4 (sleep escalation ladder — the
   biggest remaining standby lever, unblocks PB.20), ⏸ P4.20 (draw buffers — decide
   alongside P4.26 and P3.24 on the internal-RAM ledger), ⏸ P4.23 (USB PHY — run the
   bench measurement first), ⏸ P4.25 (SIMD blend — investigate only).
9. **Security, its own track, not gated on any of the above:** ✅ P4.27 (root CA bundle),
   ✅ P4.28 (password logging) and ✅ P4.29 (passphrase grace lock) all landed.
   P4.27 is the one that can break user-visible behaviour if the bundle is wrong —
   smoke-test notes sync, weather and a Telegram poll before trusting it.

Per repo convention: one `<code>` + one `<docs>` commit per item; mark items ✅ with the
measured delta as they land. And the standing lesson from five phases now — **re-verify
every `file:line` in this document against current source before editing. This file will
go stale exactly the way its predecessors did.**

---

## 6. Additions to the bench appendix

> **⚠️ All rows below remain unmeasured as of 2026-08-22.** The P4.x implementation batch landed without a bench session. Every mA value in the table header is still blank. The bench session is the highest-value next action for the entire project — run it before making any further code prioritisation decisions.

`OPTIMIZATION_BATTERY.md`'s bench table stands as written. Add these rows — each one
answers a specific question raised above, and several are decision gates:

| State / delta | Setup | Answers | mA |
|---|---|---|---|
| Idle-awake, backlight 16/16 vs 8/16 vs 1/16 | home screen, hands off | P4.3 — is the backlight the dominant awake load? | |
| Idle-awake, status bar patched vs not | P4.10 applied / reverted | P4.10 — does killing the 1 Hz redraw show up at all? | |
| Idle-awake, rotary poll 2 ms vs 20 ms | vendor delay patched | P4.11 — is the 500 Hz poll measurable? | |
| Idle-awake, IMU registered vs not | P4.22 applied / reverted | P4.22 — confirm the ~0.5–1 mA estimate | |
| Any state, `-U ARDUINO_USB_CDC_ON_BOOT` vs default | rebuild both | P4.23 — the USB PHY's real cost | |
| Fake sleep, WiFi associated vs `WIFI_OFF` | manual toggle before sleeping | P4.4 — sizes the Tier-2 payoff, and PB.20's | |
| Fake sleep, charge poll 500 ms vs 5 s | P4.8 applied | P4.8 — is the last periodic I2C visible? | |
| One Telegram poll, before vs after TLS session reuse | scope the RF window, don't average | P4.15/P4.16 — validates the ~3 mA estimate | |
| Sleep-entry energy, single vs double power-down | integrate over the transition | P4.2 — is the burst worth the fix? | |

The "vendor light sleep (PMU button)" row already in that table remains the target floor
for the fake-sleep column. Every mA of gap between them is the P4.4 ladder plus whatever
PB.2–PB.6 left behind.
