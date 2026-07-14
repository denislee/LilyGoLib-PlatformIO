# Optimization Phase 3 — Analysis & Handoff

Generated **2026-07-13** at commit `2e08813` by a four-way fresh sweep (app/UI layer,
HAL/tasks/power, Go server, flash/build config) over the **post-phase-2** tree. Every
HIGH/MEDIUM finding's file:line was spot-verified against current source before being
recorded here (the phase-1 lesson: reports go stale and agents mis-read — re-verify
again before *editing*).

**Relationship to the other docs:**
- `OPTIMIZATION_REPORT.md` — phase-1 analysis (2026-07-05), stale, history only.
- `OPTIMIZATION_PROGRESS.md` — phase-1 tracker. Its **Methodology** section (grep
  discipline, the three near-regressions) and the **hardware smoke-test checklist**
  still apply verbatim — read both before changing anything.
- `OPTIMIZATION_PHASE2.md` — phase-2 analysis/tracker. All P2.x code items are done or
  deliberately deferred; its section C ("confirmed already-optimal") still holds.
- This file — fresh findings, numbered `P3.x` (firmware) and `D6+` (server,
  continuing phase-2's D-series). Carried-forward open items are in §E.

**Baseline (post-P2.7, commit `2e08813`):** Flash 68.0 % (`firmware.bin` 2,854,928 B)
· static RAM 27.4 % (89,728 B) · OTA slot 4 MiB ⇒ **~1.15 MiB flash headroom — still
no size emergency**. The scarce resources remain **internal DRAM** (WiFi/TLS contend
for it) and **idle power**. Phase 3's biggest theme is different from phase 2's:
several fixed task stacks waste tens of KB of internal DRAM, and two findings are
outright **latent-bug fixes** (P3.8 stack-overflow risk, P3.22 wrong board variant).

**Worktree note:** at analysis time `src/apps/ui_settings.cpp` carries an uncommitted
*feature* diff (settings-tile keyboard shortcuts, +119/−21). It is unrelated to this
report; don't mistake it for optimization work in flight.

**Build discipline (unchanged):** after every change,
`pio run -e tlora_pager && pio run -e emulator_lora_pager && pio test -e native_test`.
Anything touching task loops / ISRs / stacks / boot is ⚠️ **hardware-test-required**
— add it to the smoke-test checklist in `OPTIMIZATION_PROGRESS.md`.

---

## Executive summary

| # | Finding | Impact | Risk |
|---|---|---|---|
| P3.1 | Tasks app: file write + re-read on **every checkbox toggle**, on the LVGL thread | 20–400 ms freeze per toggle | Low ✅ Done |
| P3.2 | Notes-sync log: `lv_refr_now` per drained line; log string unbounded per run | 100–600 ms stacked flushes per drain tick | Low ✅ Done |
| P3.3 | Audio-notes: SD scan + `SD.exists/mkdir` on LVGL thread at every list entry | 50–200 ms freeze per view transition | Med ✅ Done |
| P3.4 | SSH terminal trim copies ~8 KB into a `std::string` in internal DRAM per overflow | Heap churn under verbose output | Low ✅ Done |
| P3.5 | Telegram `ascii_safe` re-walks font glyph tables on every re-render | 100s of glyph lookups per new-message render | Low ✅ Done |
| P3.6 | Chat: `SD.exists` SPI probe on every mic press | 5–20 ms freeze per press | Very low ✅ Done |
| P3.7 | `loopTask` stack fixed at **30 KB** — LVGL long since moved to its own task | ~18–22 KB internal DRAM wasted, permanent | Low (measure first) |
| P3.8 | `tg_bg` worker stack **6 KB < the 8 KB TLS floor its own fg twin documents** | Latent stack overflow → heap corruption | ✅ Done |
| P3.9 | `ssh_app` task: 32 KB internal stack per session | 32 KB internal DRAM during TLS-heavy use | Med |
| P3.10 | `recorderTask` prio 12, unpinned — can preempt LVGL on core 1 | Frame drops while recording | Low fix / ⚠️ HW |
| P3.11 | `hub_probe` spawn failure leaks args + wedges `hub_probe_running` forever | Hub status indicator stuck for the session | ✅ Done |
| P3.12 | SD rail stays powered through fake sleep (board can cut it via XL9555) | ~0.5–1 mA for hours of sleep | Med / ⚠️ HW |
| P3.13 | Rotary task fake-sleep branch polls (`vTaskDelay`) instead of notify-blocking | 5 wakeups/s asleep (peers already fixed) | Very low ✅ Done |
| P3.14 | `ble_kb_ka` 3 KB stack, wants a watermark check first | ~1.5 KB RAM | Very low / ⚠️ HW (measure first) |
| P3.15 | NFC callback statics in BSS | ~116 B + hygiene | Very low ✅ Done |
| P3.16 | Double `setCpuFrequencyMhz` at boot | ~0.5 ms boot | Very low ✅ Done |
| P3.17, P3.21 | lv_conf trims: 10/11 unused widgets (arc stays — spinner dependency) + `LV_USE_FLOAT` | −26.5 KB flash (measured) | Low ✅ Done |
| P3.18 | Unused themes SIMPLE + MONO — flags flipped, but dead-code-eliminated already | 0 B measured (est. was wrong) | Low ✅ Done |
| P3.19 | lv_conf trim: I1/AL88 blends | −18.8 KB flash (measured) | Low ✅ Done |
| P3.20 | NimBLE compiled all four BLE roles; only PERIPHERAL used | −13.5 KB flash (measured) | Low ✅ Done |
| P3.22 | `boards/lilygo-t-lora-pager.json` declares `"variant": "lilygo_twatch_ultra"` | Wrong USB PID/pins if framework rebuilds | ✅ Done |
| P3.23 | Emulator defines Montserrat 21 + 40 that firmware doesn't build | Emulator masks font-fallback bugs | ✅ Done |
| P3.24 | NimBLE heap in internal SRAM (`MEM_ALLOC_MODE_INTERNAL`) | 30–60 KB internal DRAM reclaimable | Med / ⚠️ HW |
| P3.25 | BHI260 **Klio ML firmware blob = 123.7 KB of flash**; app uses no Klio features | up to −120 KB flash *if* GPIO variant works | High / ⚠️ HW, investigate only |
| P3.26 | `test_desktop` is a 2+2 placeholder; `hal/str_encode` untested | Regression-safety, zero cost | Zero ✅ Done |
| P3.27 | Partition rebalance (OTA 4→3 MiB, +2 MiB FFat) — only after size plateau | +2 MiB user storage | Deferred |
| D6 | Server chat/transcribe holds audio in ~3 simultaneous copies (~15 MiB peak on Pi) | Pi RSS spike per voice message | ✅ Done |
| D7 | Notes-sync: no retry on GitHub 429/transient 5xx (chat already has the pattern) | Whole sync aborts on one blip | ✅ Done |
| D8–D10 | Server memory trims: `buf.Grow`, release `ContentB64` after decode, `putFile` marshal copy | MiB-scale peak-RSS cuts, trivial–low | D8/D9 ✅ Done, D10 deferred |
| D11 | Graceful-shutdown window 5 s < 30–60 s upstream timeouts | In-flight sync killed on `systemctl stop` | ✅ Done |
| D12 | Server: `time.After` retry-sleep timer not stopped on ctx-done | Small | Low ✅ Done |
| D13 | Server: geoSearch trim (`httpx.Proxy` transform hook) | ~2–4 KB less per-search device parse | Low ✅ Done |
| D14 | Server cosmetics: `Cache-Control` toward device | Small | Low |

---

## A. Firmware — UI-thread & app layer

### P3.1 — Tasks app: synchronous file write + re-read on every mutation (HIGH) ✅ Done

`src/apps/ui_tasks.cpp:51` (`save_tasks` → `hw_save_preferred_file`) and `:256`
(`ui_tasks_refresh` → `hw_read_preferred_file`), both on the LVGL thread, called
back-to-back from event callbacks: checkbox toggle (`task_event_cb`, KEY), delete
(LONG_PRESSED), add/edit confirm. Every toggle blocks the UI twice — write the whole
list to FFat/SD (~10–50 ms FFat, ~50–200 ms SD), then immediately re-read it to
rebuild the UI it could have rebuilt from memory.

**Fix:** render from the live in-memory `tasks` vector (only read the file on
cold-start); debounce the write behind a ~300 ms one-shot `lv_timer` dirty flag —
the exact pattern of `ui_text_editor.cpp`'s word-count debounce. Risk: low; the
vector is already authoritative intra-session. ⚠️ Confirm persistence across an
abrupt exit inside the debounce window is acceptable (or flush in `onStop`).

**Fixed:** split `ui_tasks_refresh()` into the cold-start file read/parse and a
new `rebuild_task_list()` that rebuilds the on-screen list from the in-memory
`tasks` vector only; every mutation handler (toggle, delete, add/edit-confirm)
now calls `rebuild_task_list()` directly instead of round-tripping through the
file it just wrote. The actual `hw_save_preferred_file` write is coalesced
behind a 300 ms one-shot `lv_timer` (`schedule_save`/`save_debounce_cb`,
mirroring `ui_text_editor.cpp`'s word-count debounce); `ui_tasks_exit` calls a
new `flush_pending_save()` before tearing down the widgets `save_tasks()` reads
checked-state from, so an abrupt exit inside the debounce window still
persists the last edit. Verified in the SDL emulator (`hw_save_preferred_file`
is a `printf`-stub there, so writes are directly observable in stdout):
add/delete rebuild the list instantly with no extra read-back, mutations
coalesce into a single debounced write, and exiting ~50–100 ms after a
mutation (well under the 300 ms window) still flushes the write before
returning to the menu. `pio run -e tlora_pager` + `pio run -e
emulator_lora_pager` + `pio test -e native_test` all pass. `commit <pending>`.

### P3.2 — Notes-sync log: full display flush per drained line, unbounded log string (HIGH) ✅ Done

`src/apps/ui_notes_sync.cpp:164` — `log_append()` ends in `lv_refr_now(nullptr)`,
and `notes_sync_drain_tick` (100 ms timer) calls it once **per drained line**. N
lines in one tick ⇒ N synchronous full-screen flushes (5–30 ms each) plus N
re-copies of the ever-growing `s_log_text` into the label. `s_log_text` is unbounded
for the duration of a sync run.

**Fix:** accumulate all drained lines, then one `lv_label_set_text` + scroll per
tick and **drop `lv_refr_now` entirely** (the next `lv_timer_handler` cycle paints
it); cap `s_log_text` at ~8 000 chars trimming whole lines from the front — exactly
what `ui_chat.cpp`'s `log_append` (:130–135) already does. Risk: low.

**Fixed:** `notes_sync_drain_tick` now joins all lines drained in one tick into a
single batch and calls `log_append` once (one `lv_label_set_text` + scroll per
tick, not per line); `log_append` no longer calls `lv_refr_now` and now caps
`s_log_text` at 8 000 chars, trimming whole lines from the front — mirroring
`ui_chat.cpp`'s `log_append`. `pio run -e tlora_pager` + `pio run -e
emulator_lora_pager` + `pio test -e native_test` all pass. `commit 434bc93`.

### P3.3 — Audio-notes: SD scan on the LVGL thread at every list entry (HIGH) ✅ Done

`src/apps/ui_audio_notes.cpp:117` (`ensure_notes_dir`: `SD.exists` + `SD.mkdir`
under `ScopedSpiLock`) and `:138` (`reload_notes` → `hw_list_sd_entries`, holding
the SPI bus for the whole `openNextFile` walk). Fires at `onStart` **and on every
transition back to the list** (after stop-recording, stop-playback, delete). ~20
WAVs ≈ 50–200 ms frozen encoder/back-button each time.

**Fix:** one-shot worker + 100 ms drain timer, mirroring `weather_bg_task` /
`weather_drain_tick` (`ui_weather.cpp`); post-delete reload uses the same kick.
Risk: medium (list-build state machine refactor). ⚠️ HW test: record/stop/delete
cycling.

Phase-2 note: P2.7's file-browser analysis called this file "leave unless it shows
up in practice" for the *one-off* scan; the new observation is that it re-fires on
**every** view transition, which is a materially worse profile.

**Fixed:** split the directory walk out of `reload_notes()` into a pure
`scan_notes(std::vector<Note>&)` that touches only its argument (not the
`notes` global), so it can run off the LVGL thread. Added
`notes_scan_task`/`notes_scan_drain_tick` (one-shot `xTaskCreate` + 100 ms
`lv_timer`, `s_notes_scan_task`/`s_notes_scan_timer`/`s_notes_scan_result`
state) mirroring `weather_bg_task`/`weather_drain_tick` exactly, including the
"adopt the in-flight task instead of double-spawning" and
"leave the task to finish on its own, drop any undelivered result" exit
handling. `kick_notes_reload()` wraps the spawn (falling back to a
synchronous `reload_notes()` if `xTaskCreate` fails so the list isn't stuck
empty) and a new `show_list_view()` centralizes the platform split: on
ARDUINO it paints immediately with whatever `notes` already holds (a
"Loading..." row replaces "No notes yet" while a scan is pending) and
refreshes when the background scan drains; the emulator has no real SD or
background-task infra, so it keeps the original synchronous
scan-then-render order. `ensure_notes_dir()` (the one-time mkdir at
`onStart`) was left alone — it doesn't re-fire per transition, so it wasn't
part of the actual hot path. Also fixed a pre-existing double-scan on
delete (`pb_delete_cb` called `reload_notes()` itself and then `enter_list()`,
which called it again): delete now erases the entry from the in-memory
`notes` vector directly (same "render from memory" idiom as P3.1) so the
deleted file can't flash back into view while the async rescan is in
flight. Verified: `pio run -e tlora_pager` (Flash 68.1% / 2,855,433 B, RAM
27.5% — in line with baseline) + `pio run -e emulator_lora_pager` + `pio
test -e native_test` all pass. Manually drove the emulator under Xvfb
(mouse/keyboard via `xdotool`, screenshots via `import`): opened Settings →
Recordings, confirmed the list view renders ("No notes yet" — the
emulator's `hw_list_sd_entries` stub never returns entries for
`/mental_notes`, so this path was already unexercisable pre-fix), backed
out to the home screen and re-entered twice with no crash and no stale
state in the log. The async task/timer path itself (`#ifdef ARDUINO`-only)
and the record/stop/delete cycling remain ⚠️ **hardware-test-required** —
carried into the smoke-test checklist. `commit fce0e8e`.

### P3.4 — SSH terminal trim: ~8 KB `std::string` copy in internal DRAM (MED) ✅ Done

`src/apps/ui_ssh.cpp:905–908` — when `term_len_ > kTermMax` (8 000), the trim path
does `std::string cur = lv_textarea_get_text(term_)` (heap copy of up to ~8 KB in
internal DRAM, the WiFi/TLS pool) just to compute a cut offset. Under verbose output
(`dmesg`, `find /`) this repeats continually and fragments the heap.

**Fix:** compute the cut offset on the returned `const char*` directly and call
`lv_textarea_set_text(term_, raw + offset)` — LVGL copies internally; zero
intermediate allocation. Mind UTF-8 boundaries when picking the offset. Risk: low.

**Fixed — not as literally proposed above.** Traced `lv_textarea_set_text` →
`lv_label_set_text` → `set_text_internal` (`lv_label.c`): when the `txt` argument
isn't the label's *own* current buffer pointer by identity, it **frees the old
buffer first**, then `strcpy`s from `txt`. A `raw + offset` slice (this doc's
literal suggestion) aliases memory `lv_textarea_set_text` frees before reading it
— a use-after-free that happens to often "work" only because nothing reallocates
between the free and the read, which is exactly the kind of thing that breaks
first under ESP-IDF heap poisoning or a future LVGL/allocator change. Implemented
instead: scan the live buffer in place with `strlen`/`memchr` (no copy) to find
the cut point — mirroring UTF-8 continuation-byte skipping for the no-newline
fallback — then construct **one** owned `std::string` from only the surviving
tail and hand that to `lv_textarea_set_text`. This still cuts the work roughly in
half versus the original (which copied the *whole* buffer via the `std::string`
constructor and then `erase()`d the discarded prefix) without aliasing a buffer
LVGL is about to free. Also hardened the size-tracking edge case: if the tracked
`term_len_` ever drifts above `kTermMax` while the real buffer (via `strlen`)
isn't, resync `term_len_` instead of computing a nonsensical cut. Verified the
trim/cut/UTF-8-boundary logic against a standalone harness (line-boundary cut,
hard cut with no newline, UTF-8 continuation-byte skip, no-op when under the
cap) before touching device code. `pio run -e tlora_pager` (Flash 68.1% /
2,855,457 B, RAM 27.5% — in line with baseline) + `pio run -e
emulator_lora_pager` + `pio test -e native_test` all pass. No SSH hardware path
to drive from the emulator; the trim only fires past 8 000 buffered bytes, so it
stays ⚠️ **hardware-test-required** (verbose remote output, e.g. `dmesg`/`find /`)
— added to the smoke-test checklist. `commit 9863857`.

### P3.5 — Telegram `ascii_safe` re-sanitizes every message on every render (MED) ✅ Done

`src/apps/ui_telegram.cpp:352` — `ascii_safe()` calls `lv_font_get_glyph_dsc`
(:380, fallback-chain walk) per non-ASCII codepoint, and `render_msgs` calls it for
every sender + text (:621, :649) each time the message signature changes. Emoji-heavy
chats ⇒ hundreds of glyph lookups on the LVGL thread per poll that lands new messages.

**Fix:** sanitize once at parse time in the worker (`tg_bg_tick` message build) and
store `safe_text`/`safe_from` alongside the raw fields; render uses the cached
strings. The existing `msgs_signature` gate already bounds recomputation. Risk: low;
emulator-verifiable.

**Fixed:** `Message` gained `safe_from`/`safe_text` fields; `tg_parse_msgs` (the
`tg_fg_task` HTTPS-worker's message parser, not `tg_bg_tick` — that background
timer only fetches unread counts, never message bodies) now calls `ascii_safe()`
once per message right after parsing, off the LVGL thread. `render_msgs()` reads
`it->safe_from`/`it->safe_text` instead of recomputing. Checked one thread-safety
wrinkle before landing this: `ascii_safe` → `get_telegram_font()` lazily
first-time-initializes the Inter+emoji font variants
(`ui_tools.cpp`'s `init_inter_emoji_fonts`, guarded only by a plain bool, no
lock). `show_chat()` always calls `get_telegram_font()` synchronously on the LVGL
thread while building the chat view — *before* it kicks the first message fetch —
so the worker can never be the first caller and there's no init race in practice.
Emulator can't exercise this path (the whole `tg_fg_task`/`tg_parse_msgs` worker
is `#ifdef ARDUINO`-only; the emulator's Telegram view has no live network path
to populate `s_msgs`), so verification was build-only: `pio run -e tlora_pager`
(Flash 68.1%, RAM 27.5% — in line with baseline) + `pio run -e
emulator_lora_pager` + `pio test -e native_test` all pass. `commit 8048ef8`.

### P3.6 — Chat: SD probe on every mic press (LOW) ✅ Done

`src/apps/ui_chat.cpp:664` — `ensure_chat_dir()` runs `SD.exists` (+ conditional
`mkdir`) on the LVGL thread on each mic press. **Fix:** `static bool s_chat_dir_ok`
— call `SD.mkdir(CHAT_DIR)` once per session (it fails harmlessly if present) and
skip afterwards. Risk: very low.

**Fixed:** `ensure_chat_dir()` now short-circuits on a static `s_chat_dir_ok` flag
once the SD dir has been ensured, and calls `SD.mkdir(CHAT_DIR)` unconditionally
(dropping the `SD.exists` probe) instead of on every mic press. `pio run -e
tlora_pager` + `pio run -e emulator_lora_pager` + `pio test -e native_test` all
pass. `commit f935d2a`.

---

## B. Firmware — task stacks, power, correctness

Complete task inventory (name / stack / prio / core / origin) is at the end of this
section — keep it current when adding tasks.

### P3.7 — `loopTask` stack fixed at 30 KB (HIGH — biggest single internal-RAM lever)

`src/hal/system.cpp:187–189` — `getArduinoLoopTaskStackSize()` returns `30 * 1024`.
That size predates the LVGL split: rendering moved to its own 16 KB `lvgl` task
(`hal/lvgl_task.cpp`), leaving `loop()` (factory.ino) with NTP retry, CPU-freq
bookkeeping, `instance.loop()` and `delay()` — realistic depth 4–8 KB. ~18–22 KB of
internal DRAM is reserved forever for nothing.

**Fix:** instrument `uxTaskGetStackHighWaterMark(NULL)` in `loop()` on a warm system
(WiFi up, NTP in flight), then set watermark × 1.5 rounded to 4 KB — likely 12 KB,
16 KB if `instance.loop()` surprises. Risk: low *with* the measurement; do not guess.
⚠️ HW-test-required (watermark reading is the test).

**Still blocked, re-confirmed 2026-07-14:** no LilyGo device attached this session
(`pio device list` shows only virtual `ttyS*` ports, `lsusb` has no matching VID) —
can't take the watermark reading the fix depends on. Not guessing the number per
this doc's own rule. Picked up P3.15/P3.16 instead (pure code, zero-HW-dependency
items from the same execution-order batch); P3.14 stays open for the same reason as
P3.7 — its fix also wants a watermark check first.

### P3.8 — `tg_bg` stack 6 KB is below the TLS floor its own twin documents (BUG) ✅ Done

`src/apps/ui_telegram.cpp:1706` — `xTaskCreate(tg_bg_task, "tg_bg", 6144, …)`. The
foreground twin at `:854` uses **8192** with the in-code comment (:852) "8 KB stack:
the mbedtls TLS handshake + cert chain dominates". `tg_bg_task` performs the same
HTTPS fetch through the same stack. A long cert chain can push depth past 6 KB;
FreeRTOS stack overrun here corrupts adjacent heap → non-deterministic failures that
won't look telegram-related. (Both tasks share the single `s_bg_task` worker slot by
design — `:190` comment — so only the size differs, not the lifecycle.)

**Fix:** `6144 → 8192`. One line. Do this first of everything in this file. ⚠️ HW:
stress-poll with a Let's Encrypt-style chain + `uxTaskGetStackHighWaterMark`.

### P3.9 — `ssh_app`: 32 KB internal stack per session (HIGH while active)

`src/apps/ui_ssh.cpp:200–201` — on-demand task (fine when idle), but an active
session pins 32 KB of internal DRAM exactly while mbedTLS handshake buffers peak.

**Fix:** (a) measure watermark during a real session (auth + channel + bulk output)
and trim toward ~20 KB; or (b) move the stack to PSRAM via `xTaskCreateWithCaps`
(needs `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`; check the Arduino-ESP32 sdkconfig
before assuming) — frees all 32 KB at a small context-switch latency cost. Risk:
medium — measure before shrinking. ⚠️ HW-test-required.

### P3.10 — `recorderTask`: priority 12, unpinned — can preempt LVGL (MED-HIGH)

`src/hal/audio.cpp:534` — `xTaskCreate(recorderTask, "app/rec", 8*1024, NULL, 12, …)`
is unpinned; if scheduled to core 1 it outranks the `lvgl` task (prio 8) and blocks
it for codec-frame durations (~30 ms) ⇒ visible UI hitches while recording. (Same
shape as known-open P2.6 `playerTask` at `:328` — treat the two together.)

**Fix:** pin to core 0 (`xTaskCreatePinnedToCore(…, 0)`); afterwards measure
watermark and consider 8 KB → 4–6 KB. Risk: low for pinning. ⚠️ HW: record while an
animated screen renders; verify no dropped frames/garbled audio.

### P3.11 — `hub_probe` spawn failure wedges the hub indicator + leaks 8 B (BUG) ✅ Done

`src/core/system.cpp:296–313` — `hub_probe_running = true` is set, then the args are
`malloc`'d in an immediately-invoked lambda **inside the `xTaskCreate` argument
list**, and the return value of `xTaskCreate` is ignored. If the spawn fails (heap
pressure — exactly when a probe is interesting), the flag is never reset (task body
owns the reset) → every future probe is skipped for the session, and the args leak.
`malloc` failure would also crash: `args[0]=…` dereferences unchecked.

**Fix:** hoist the malloc, null-check it, check `xTaskCreate != pdPASS` → `free(args);
hub_probe_running = false;`. Risk: none — pure guard code.

### P3.12 — SD rail powered through fake sleep (MED, wants a bench)

`hw_power_down_all()` (`src/hal/system.cpp`) deliberately skips
`powerControl(POWER_SD_CARD, false)` citing remount overhead — but the pager's
XL9555 expander can cut SD power and the vendor's own `lightSleep()` does. An idle
SD draws ~0.5–1 mA; fake sleep lasts hours ⇒ 10–20 mAh per 10 h sleep on a ~1000 mAh
battery.

**Fix:** cut the rail in `hw_power_down_all()`, re-`SD.begin()` on wake, with a
quiesce gate (no bulk/prune/sync in flight, writes flushed). Risk: medium — remount
reliability and write-in-flight corruption need care. ⚠️ HW-test-required: sleep with
SD mounted, wake, verify FS intact. Bench-measure the actual mA first to confirm the
win justifies the remount complexity.

### P3.13 — Rotary task fake-sleep branch still polls (LOW) ✅ Done

`src/hal/rotary_task.cpp:69–70` — the fake-sleep branch is
`vTaskDelay(kFakeSleepIdleMs)` where keyboard/NFC/charge/lvgl all use
`ulTaskNotifyTake(…, 200 ms)` + a wake kick. 5 wakeups/s at near-max priority during
sleep. **Fix:** mirror `keyboard_task`: add `hw_rotary_task_notify_wake()`, call it
from `ui_resume_timers()` (`ui_main.cpp`), block on notify. Risk: very low; no HW
test needed beyond the existing checklist's wake-latency item.

**Fixed:** re-verified the file:line and the "peers already fixed" premise against
current source before editing — `keyboard_task.cpp`'s fake-sleep branch was the
exact pattern to mirror (`ulTaskNotifyTake` + reset, no `xLastWakeTime` bookkeeping
needed here since `rotary_task_fn`, unlike `keyboard_task_fn`, has no
`vTaskDelayUntil` cadence outside fake-sleep — it just calls the blocking
`instance.getRotary()` directly). Added `hw_rotary_task_notify_wake()` (mirrors
`hw_keyboard_task_notify_wake()`: `if (s_task) xTaskNotifyGive(s_task);`, plus
no-op stubs in the `!USING_INPUT_DEV_ROTARY` and `!ARDUINO` branches), declared it
in `core/system_hooks.h` alongside its keyboard/LVGL siblings, and swapped the
fake-sleep branch's `vTaskDelay(kFakeSleepIdleMs)` for
`ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFakeSleepIdleMs))`. `ui_resume_timers()`
(`ui_main.cpp`) now also calls `hw_rotary_task_notify_wake()` alongside the
existing LVGL/keyboard wake kicks. `pio run -e tlora_pager` (RAM 27.4% / 89,824 B,
Flash 66.7% / 2,796,653 B — in line with baseline) + `pio run -e
emulator_lora_pager` (unaffected — `rotary_task.cpp` is `#ifdef ARDUINO`-only) +
`pio test -e native_test` all pass. ⚠️ Wake-latency verification (first scroll/
click after fake-sleep wake registers immediately) needs real hardware — added to
`OPTIMIZATION_PROGRESS.md`'s smoke-test checklist alongside the existing P2.1
keyboard check it mirrors. `commit 08d3b93`.

### P3.14 — `ble_kb_ka` stack 3 KB for a 1-line loop (LOW)

`src/hal/wireless.cpp:576` — keepalive task does `isConnected()` once a second and a
tiny HID send every 25 s; ~1.5 KB wasted. **Fix:** 3072 → 1536 after a watermark
check. ⚠️ trivial HW check while paired.

**Still open 2026-07-14:** blocked on the same hardware gap as P3.7 — the fix is
explicitly "after a watermark check," and there's no device attached this session
to take one. Do this whenever a hardware session happens (pair with P3.7/P3.9/P3.10).

### P3.15 — NFC callback `static` locals live in BSS forever (LOW) ✅ Done

`src/hal/peripherals.cpp:52–57` — six `static` locals (~116 B incl. a `String` and a
two-`std::string` struct) inside `ndef_event_callback` are permanent internal-RAM
residents used only while a tag is being read. **Fix:** drop `static` (the 4 KB
`nfc_reader` task stack absorbs them; everything is reinitialized per call). Risk:
very low.

**Fixed:** dropped `static` from all six locals (`devInfoData`, `bufAarString`,
`url`, `text`, `msg`, `params`); dropped the now-pointless `msg = ""` reset (a
freshly constructed `String` is already empty). Each is populated fresh from
`data` per the `switch (id)` branch before use and never read across calls, so
there's no cross-invocation state to preserve. `pio run -e tlora_pager` (Flash
66.7% / 2,796,593 B, RAM 27.4% / 89,824 B — in line with baseline) + `pio run -e
emulator_lora_pager` + `pio test -e native_test` all pass. No NFC hardware to
exercise the callback itself; behavior is unchanged by construction (same values,
now stack-allocated), so no HW test is needed beyond the existing NFC checklist
item. `commit 0bfa9e4`.

### P3.16 — Double `setCpuFrequencyMhz(240)` at boot (LOW) ✅ Done

`src/factory.ino:67` unconditional, then `:77` from NVS. Line 67 exists so early boot
isn't at a low default while loading settings — but when `cpu_freq_mhz == 240` the
second call redundantly re-inits the PLL (~0.5 ms). **Fix:** guard line 77 with
`if (settings.cpu_freq_mhz != 240)` (keeping line 67 as the fast-boot default), or
drop line 77 if boot-at-default-freq for those few ms is acceptable. Cosmetic.

**Fixed:** guarded the `:77` call with `if (settings.cpu_freq_mhz != 240)`, keeping
line 67's unconditional 240 MHz as the fast-boot default. `pio run -e tlora_pager` +
`pio run -e emulator_lora_pager` (emulator has no `factory.ino`/`setup()` path —
structurally unaffected) + `pio test -e native_test` all pass. No behavioral
difference to observe beyond boot timing (~0.5 ms), which isn't measurable from the
emulator; stays off the smoke-test checklist as genuinely cosmetic. `commit e6922f1`.

### Informational (vendor code — document, don't patch)

- Vendor `rotary` task (`lib/LilyGoLib/src/LilyGo_LoRa_Pager.cpp:483`): prio 10 >
  LVGL 8, unpinned, 500 Hz. Only chase if frame-jitter during scroll is measured
  (post-init `vTaskCoreAffinitySet` would be the lever).
- `lv_display_set_flush_wait_cb` (`LV_Helper_v9.cpp`) registers a `log_d` no-op that
  is also unreachable (flush is synchronous). Harmless.

### Task inventory (verified 2026-07-13)

| Task | Stack B | Prio | Core | Origin |
|---|---|---|---|---|
| `loopTask` | 30 720 **(P3.7)** | 1 | 1 | framework via `hal/system.cpp:189` |
| `lvgl` | 16 384 | 8 | 1 | `hal/lvgl_task.cpp` |
| `kb_reader` | 4 096 | max−2 | 0 | `hal/keyboard_task.cpp` |
| `rotary_reader` | 3 072 | max−2 | 0 | `hal/rotary_task.cpp` |
| `nfc_reader` | 4 096 | max−3 | 0 | `hal/nfc_task.cpp` |
| `charge_ind` | 4 096 | 3 | 0 | `hal/charge_task.cpp` |
| `rotary` (vendor) | 2 048 | 10 | any | vendor `LilyGo_LoRa_Pager.cpp:483` |
| `app/play` | 8 192 (P2.6 open) | 12 | any | `hal/audio.cpp:328` |
| `app/rec` | 8 192 **(P3.10)** | 12 | any | `hal/audio.cpp:534` |
| `ssh_app` | 32 768 **(P3.9)** | 5 | 0 | `apps/ui_ssh.cpp:201` |
| `tg_fg` | 8 192 | 2 | any | `apps/ui_telegram.cpp:854` |
| `tg_bg` | 8 192 ✅ (P3.8) | 2 | any | `apps/ui_telegram.cpp:1706` |
| `tg_inet` | 3 072 | 1 | any | `apps/ui_telegram.cpp:451` |
| `hub_probe` | 3 072 **(P3.11)** | 1 | any | `core/system.cpp:298` |
| `inet_chk` / `inet_tst` | 4 096 | 1 | any | `menu_app.cpp` / `settings_connectivity.cpp` |
| `chat_send` | 8 192 | 1 | 0 | `apps/ui_chat.cpp` |
| `nsync_bg` / `wx_bg` / `wx_city` / `tg_favs` | 8 192 | 2 | any | respective apps |
| `notes_prewarm` / `notes_prune` | 8 192 | 1–2 | any | `hal/notes_crypto.cpp` / `hal/storage.cpp` |
| `ble_kb_ka` | 3 072 **(P3.14)** | 1 | any | `hal/wireless.cpp:576` |

---

## C. Flash / build / config

All lv_conf edits are in `lib/LilyGoLib/src/lv_conf.h` (vendored — changes live in
this repo's copy). Sizes measured via `xtensa-esp32s3-elf-nm --size-sort` + the
linker map on the current `.pio/build/tlora_pager/firmware.elf`.

### P3.17 — 11 LVGL widgets enabled and unused; 8 of them cannot be gc'd (−15–20 KB, LOW) ✅ Done

Zero `lv_{arc,calendar,chart,led,line,roller,scale,win,animimg,imagebutton,canvas}_create`
call sites in `src/` (grep-verified), yet arc/calendar/chart/led/line/roller/scale/win
are pulled into the image because `lv_theme_default.c.o` references their class
objects (confirmed in firmware.map; `lv_chart_event` alone is 3.6 KB, nm-visible total
~12.8 KB + rodata + theme branches). `lv_conf.h:586–673`: set `LV_USE_ARC/CALENDAR/
CANVAS/CHART/ANIMIMG/IMAGEBUTTON/LED/LINE/ROLLER/SCALE/WIN` → 0. Note LVGL's dropdown
does **not** depend on the roller flag (own internal class). Canvas/animimg/
imagebutton are already gc'd — flipping them is hygiene only.

**Fixed — 10 of 11, not arc.** Re-verifying the zero-call-site premise against current
source before editing (per this doc's own methodology) surfaced a dependency the
original grep missed: `lv_spinner.h` has a hard `#if LV_USE_ARC == 0 / #error` — and
`lv_spinner_create` **is** called, twice, in `ui_tools.cpp` (the shared loading-popup
widget every app uses via `ui_loading_t`). Flipping `LV_USE_ARC` to 0 fails the build
immediately (`pio run -e tlora_pager`). `LV_USE_ARC` stays `1` with a comment
explaining why; the other 10 (`CALENDAR/CANVAS/CHART/ANIMIMG/IMAGEBUTTON/LED/LINE/
ROLLER/SCALE/WIN`) went to 0 as planned — none of them are transitively required by
anything else enabled. `pio run -e tlora_pager` + `pio run -e emulator_lora_pager`
(emulator doesn't read `lv_conf.h` at all — `LV_CONF_SKIP` + `lv_conf_simple.h` in
`[env_emulator]` — so it's structurally unaffected; rebuilt clean anyway) + `pio test
-e native_test` all pass. Manually swept the emulator under Xvfb (mouse/keyboard via
`xdotool`, screenshots via `import`): main menu, Settings list, Display & Backlight
(slider), Connectivity (switches + WiFi-On sub-rows), Storage (list rows + result
modal) all render with no corruption. `commit d54184b` (combined with P3.21 below,
same commit — both are one lv_conf.h edit pass per the execution-order note).

### P3.18 — Unused themes SIMPLE + MONO linked via `lv_init` deinit refs (−8.4 KB, LOW) ✅ Done (0 B measured — premise was wrong)

`lv_conf.h:694,697`. Only `lv_theme_default_init` is called (`ui_theme.cpp:71`); mono
(~5.2 KB) + simple (~3.2 KB) are retained because `lv_init.c.o` references their
deinit functions unconditionally. Set both to 0.

**Fixed, but the estimate doesn't hold.** Re-verified the file:line before editing
(per this doc's own methodology) and the "unconditionally referenced" premise
doesn't survive: `lv_init.c`'s `lv_deinit()` does call `lv_theme_simple_deinit()` /
`lv_theme_mono_deinit()` under `#if LV_USE_THEME_SIMPLE` / `#if LV_USE_THEME_MONO`
guards — but `lv_deinit()` itself is never called anywhere in `src/` or the vendored
lib (grep-verified). With `-ffunction-sections`/`--gc-sections` in effect, the whole
deinit chain — `lv_theme_default_deinit` included — is already unreachable dead code
and was being stripped by the linker regardless of these flags (confirmed via `nm
--size-sort` on `firmware.elf`: no `theme_default_deinit`/`theme_simple_deinit`/
`theme_mono_deinit`/`lv_deinit` symbols present even with both flags at `1`). Set
both to 0 anyway — harmless, and it removes the dead premise so a future pass
doesn't re-derive the same wrong estimate — but measured flash delta is **0 bytes**
(2,828,921 B before and after, byte-for-byte, confirmed by toggling the flags back
and forth and rebuilding both ways). `pio run -e tlora_pager` + `pio run -e
emulator_lora_pager` (unaffected — doesn't read `lv_conf.h`) + `pio test -e
native_test` all pass. `commit 34efdf2`.

### P3.19 — I1 + AL88 software-blend paths unused (−18.8 KB, LOW) ✅ Done

`lv_conf.h:152,154`. `blend_image_to_i1` (4.4 KB) + `blend_image_to_al88` (2.9 KB)
are in the ELF; no I1/AL88 image source or descriptor exists in `src/` (display is
RGB565; fonts are 4-bpp). Risk is that some LVGL feature blends into these formats
internally — verify with a full emulator pass (all screens) after flipping; revert on
any render corruption.

**Fixed.** Re-verified before editing: grepped for `LV_COLOR_FORMAT_I1`/`AL88`/`L8`
consumers across the vendored LVGL source — the only non-blend consumers are
`lv_qrcode.c`/`lv_barcode.c` (I1 draw buffers) and `lv_canvas.c` (I1 support), and
`LV_USE_QRCODE`/`LV_USE_BARCODE`/`LV_USE_CANVAS` are all already `0` in this repo's
`lv_conf.h`; the one AL88-producing path (`lv_draw_sw_img.c`'s image-transform
helper, `L8 → AL88` intermediate for rotate/zoom) only fires for `LV_COLOR_FORMAT_L8`
source images, and `src/` has zero `lv_image_dsc_t`/`lv_img_dsc_t` bitmap assets at
all (only generated fonts) — confirmed with a repo-wide grep before touching the
flags. Set both `LV_DRAW_SW_SUPPORT_AL88` and `LV_DRAW_SW_SUPPORT_I1` to `0`.
Measured: `tlora_pager` flash 2,815,401 B → 2,796,645 B (−18,756 B, better than the
−7.3 KB estimate — the estimate only counted the two `blend_to_*.c` object files,
not the extra AL88/I1 case arms compiled into every *other* `blend_to_*.c` TU), RAM
unchanged. `pio run -e tlora_pager` + `pio run -e emulator_lora_pager` (unaffected —
`LV_CONF_SKIP` means the emulator never reads this `lv_conf.h`) + `pio test -e
native_test` all pass.

Did the required full emulator render pass (Xvfb + `xdotool` + `import`, mouse-driven):
main menu, Settings list, Display & Backlight (slider), Connectivity (toggles),
Weather (icons), Storage (copy/upload/trash icons), Fonts — all render with no
corruption. Mid-pass hit a real SDL segfault, tracked down via an isolated A/B
rebuild (toggling *only* `lv_conf.h` while holding everything else fixed) to
**`src/apps/ui_settings.cpp`'s pre-existing uncommitted settings-tile
keyboard-shortcut diff** (the one this doc's intro "Worktree note" already flags as
unrelated feature work in flight) — reproduces identically with `lv_conf.h` at
baseline (`LV_DRAW_SW_SUPPORT_I1/AL88 = 1`), so it is **not** a P3.19 regression.
Repro: from the main menu, open Settings, jump into a subpage via its keyboard-
shortcut hotkey (e.g. `d` for Display & Backlight), then click the back arrow —
crashes intermittently within a couple of back-navigations. Left unfixed
(out of scope for this optimization pass, and it's the user's own in-flight code);
flagged to the user directly rather than silently patched. `commit daeba61`.

### P3.20 — NimBLE compiles all four BLE roles; only PERIPHERAL is used (−5–15 KB + RAM, LOW) ✅ Done

No `_DISABLED` role flags anywhere in build config; `src/hal/wireless.cpp` uses only
the HID-peripheral API (no `NimBLEClient`/`NimBLEScan` hits in `src/`). Add to
`[env:tlora_pager]` build_flags: `-D CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED` and
`-D CONFIG_BT_NIMBLE_ROLE_OBSERVER_DISABLED` (the officially supported trim).

**Fixed — landed in `env_arduino`, not `env:tlora_pager`.** Re-verified the
no-client/no-scan premise before editing: grepped `src/` (only `NimBLEDevice`/
`NimBLEServer`/`NimBLEConnInfo` includes and calls in `wireless.cpp`) and the
vendored `.pio/libdeps/tlora_pager/ESP32 BLE Keyboard Fork` sources (no
`NimBLEScan`/`NimBLEClient`/`NimBLEAdvertisedDevice`/`createClient` hits either)
— confirmed peripheral-only. `wireless.cpp`'s NimBLE usage isn't gated by any
`ARDUINO_T_*` board macro, so both flags went into `[env_arduino]`'s shared
`build_flags` (same block as the `RADIOLIB_EXCLUDE_*` list) rather than
`tlora_pager`-only, covering all three hardware targets from one edit. Checked
`nimconfig.h` directly to confirm `CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED` /
`_OBSERVER_DISABLED` are the actual guard macros the header checks (not just
inferred from the doc). Measured: `tlora_pager` flash 2,828,921 B → 2,815,401 B
(−13,520 B), RAM unchanged. `pio run -e tlora_pager` + `pio run -e
emulator_lora_pager` (unaffected — the SDL2 build has no BLE stack at all) +
`pio test -e native_test` all pass. `twatchs3` was spot-checked too but fails to
build for unrelated pre-existing reasons (`settings_imu_debug.cpp`
`BoschSensorInfo`/`info` out of scope, `ui_ssh.cpp` missing `libssh_esp32.h`),
reproduced identically with this change stashed out — confirmed not a
regression, and `twatchs3` isn't part of this repo's stated build-discipline
loop anyway. `commit ce570c2`.

### P3.21 — `LV_USE_FLOAT` only feeds the (unused) arc widget (−1–2 KB, LOW after P3.17) ✅ Done

`lv_conf.h:469`. `lv_arc.c.o` is the sole puller of `__divsf3` (map-verified). After
P3.17 removes arc, set `LV_USE_FLOAT 0` (`lv_value_precise_t` → int32). Do it in the
same pass as P3.17, not before.

**Fixed — arc stayed enabled (see P3.17), set anyway.** `lv_arc.c` uses
`lv_value_precise_t` throughout but only two blocks are actually `#if LV_USE_FLOAT`-
guarded (checked against the vendored source before flipping) — the type is just an
`int32_t`/`float` typedef, arc has no hard requirement on which. No file in `src/`
references `LV_USE_FLOAT` or `lv_value_precise_t` directly. `LV_USE_MATRIX` (the only
other consumer, requires `LV_USE_FLOAT=1`) was already `0`, so no conflict. Set to
`0`. Combined flash delta for P3.17+P3.21 together: 2,855,457 B → 2,828,921 B
(−26,536 B) on `tlora_pager`; RAM unchanged. Same build/emulator verification as
P3.17. `commit d54184b`.

### P3.22 — Pager board JSON declares the T-Watch-Ultra variant (BUG, fix regardless) ✅ Done

`boards/lilygo-t-lora-pager.json:21` — `"variant": "lilygo_twatch_ultra"`. The
framework core compiles *its* `pins_arduino.h` from this field (USB PID 0x82D4 vs
0x8227, product string, display dims, SD CS). Currently masked by the prebuilt
framework cache; any clean framework rebuild bakes in the wrong USB identity. Fix to
`"lilygo_tlora_pager"` (directory exists in `variants/`). Zero flash impact —
correctness. ⚠️ After fixing, verify a clean build still boots + USB-enumerates as
the pager.

**Fixed:** `variant` → `lilygo_tlora_pager`. `pio run -e tlora_pager` +
`pio run -e emulator_lora_pager` + `pio test -e native_test` all pass. ⚠️ USB
re-enumeration after a *clean* framework rebuild still wants a hardware check —
carried into the smoke-test checklist.

### P3.23 — Emulator/hardware font drift: Montserrat 21 + 40 (ZERO risk parity fix) ✅ Done

`platformio.ini:249,259` (`[env_emulator]` block) define `LV_FONT_MONTSERRAT_21=1`
and `_40=1`; firmware has 40 = 0 (phase-2 P2.10) and no 21 at all. The emulator can
render sizes the device silently falls back on. Delete both lines.

**Fixed:** both lines removed from `[env_emulator]`. Emulator build + native tests
pass; no source references either size (grep-verified before the edit).

### P3.24 — NimBLE heap lives in internal SRAM (30–60 KB reclaimable, MED)

NimBLE defaults to `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL` (nimconfig.h); no
override in build flags. Adding `-D CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=1`
moves the BLE stack heap to PSRAM — for an occasional-keystroke HID device the PSRAM
latency is irrelevant. ⚠️ HW-test: pairing + key latency + reconnect.

### P3.25 — BHI260 Klio ML firmware blob: 123.7 KB of flash (INVESTIGATE ONLY, HIGH risk)

`bosch_bhi260_klio_firmware_image` = 123 696 B in `.flash.rodata` (nm-verified) —
the single largest non-font object in the image. Selection chain:
`variants/lilygo_tlora_pager/pins_arduino.h:89` `USING_XL9555_EXPANDS` →
`LilyGo_LoRa_Pager.cpp:1081` picks the **Klio** (activity-ML) BHI260 firmware over
the plain GPIO one. `src/hal/sensors.cpp` uses only accel/rotation/orientation — no
Klio features. *But* the XL9555 IO expander (audio-amp enable etc.) is routed through
the BHI260, and the vendor's Klio-for-XL9555 mapping may be a hardware-validation
decision. **Do not swap blindly.** Dedicated investigation: build with
`bosch_bhi260_gpio.h`, flash a test device, verify every XL9555-driven rail (amp,
backlight, SD) still switches; measure the size delta. Potential −100–120 KB;
bricking-adjacent if wrong.

### P3.26 — Native test gap: `test_desktop` is a placeholder; `hal/str_encode` untested (quality) ✅ Done

`test/test_desktop/test_main.cpp` asserts 2+2 (phase-1 §3.5 leftover). The phase-2
`hal/str_encode.cpp` (json_escape/b64/url_encode — pure logic, three consumers) has
no tests. Replace the placeholder with a str_encode round-trip suite and add
`+<hal/str_encode.cpp>` to the `native_test` build_src_filter. Zero risk.

**Fixed — 2 of 3 functions, not b64.** Re-verified the "pure logic" premise before
writing tests: `hal::base64_encode` (`hal/str_encode.cpp:53`) is `#ifdef
ARDUINO`-gated (implemented via `mbedtls_base64_encode`) and every call site is
itself under `#ifdef ARDUINO` — it has no portable form and mbedtls isn't linked
into `native_test`, so it stays untested by construction (documented in the new
test file's header, not silently dropped). `json_escape` and `url_encode` have no
such gate and are genuinely pure. Replaced `test_desktop`'s 2+2 placeholder with 11
Unity cases: `json_escape` (empty, plain passthrough, quote/backslash, named
control chars `\n\r\t`, the `\u00XX` fallback for other control bytes, UTF-8 bytes
passing through un-escaped) and `url_encode` (empty, unreserved-charset
passthrough, space/reserved-char percent-encoding, UTF-8 byte-by-byte
percent-encoding, uppercase hex digits). Added `+<hal/str_encode.cpp>` to
`[env:native_test]`'s `build_src_filter` in `platformio.ini`. `pio test -e
native_test` — 28 cases total (11 new + the existing 17) all pass; `pio run -e
tlora_pager` + `pio run -e emulator_lora_pager` also re-verified green (this change
doesn't touch firmware source, only test infra + build config, but re-ran per this
doc's standing build discipline). Zero HW dependency, zero behavioral risk — test-
only change. `commit 9ff3df8`.

### P3.27 — Partition rebalance (DEFERRED until flash plateau)

OTA slots 2 × 4 MiB vs a 2.85 MB image (partitions.csv). If/when P3.17–P3.25 and the
P2.11 font decisions land the image comfortably under 3 MiB, shrinking OTA slots to
2 × 3 MiB frees +2 MiB for FFat. Breaks the OTA chain (partition-table reflash) — do
not do opportunistically.

**Re-confirmed optimal this pass (don't re-investigate):** LVGL heap + display
buffers in PSRAM; audio/NFC buffers in PSRAM; cJSON hooks live; montserrat set
10–32+48 all have call sites; all 22 generated fonts referenced; RadioLib excludes;
Makefile/tools clean; coredump partition sized right.

---

## D. Go server (lilyhub) — continues phase-2's D-series

`gofmt -l` and `go vet` clean at analysis time. D1–D4 (SSRF, map caps, traversal,
timeouts) verified still in place; outbound contexts/body-closing/goroutines
re-checked clean; `http.Client` reuse confirmed in all four handlers.

### D6 — Voice chat holds the audio in ~3 copies simultaneously (~15 MiB peak, HIGH for a Pi) ✅ Done

`internal/chat/chat.go:185` (`DecodeString` of `req.AudioB64`), `:239` (multipart
`bytes.Buffer`), `:261` (`fw.Write(audio)`). For a near-max upload: base64 string
(~6 MiB) + decoded bytes (~4.5 MiB) + multipart buffer (~4.5 MiB) live together
during the Groq upload. **Fix:** pass the base64 string into `transcribe` and
stream-decode straight into the multipart writer via
`io.Copy(fw, base64.NewDecoder(base64.StdEncoding, strings.NewReader(audioB64)))`;
zero `req.AudioB64 = ""` right after. Cuts peak by ~a third; combined with D8 the
buffer is also single-allocation.

**Fixed:** `transcribe` now takes the base64 string and stream-decodes into the
multipart writer as prescribed; `chat()` zeroes `req.AudioB64` right after the
call; `buf.Grow` (D8) re-derives its size from the base64 length via
`base64.StdEncoding.DecodedLen`. Invalid base64 now surfaces mid-copy instead of
up front, so it's wrapped in a new `errBadAudio` type (`errors.As`-checked in
`chat()`) to keep the existing 400-vs-502 split intact. `commit 9d33b16`, with
tests covering the streamed bytes, the bad-base64 path, and the end-to-end
handler status code.

### D7 — Notes-sync has no retry on GitHub 429/transient 5xx (MED) ✅ Done

`internal/notessync/notessync.go` `listRemote`/`putFile` are single-attempt while
`chat.do()` already implements 3-attempt context-aware backoff for exactly these
codes. One GitHub blip aborts the whole (serial) sync back to the device. **Fix:**
reuse the chat backoff pattern (200→400→800 ms) on 429/5xx for `putFile`, one retry
for `listRemote`. Low risk; sync is additive/idempotent.

**Fixed:** added `doWithRetry`, mirroring `chat.go`'s backoff/`GetBody`-replay
pattern but returning the final status instead of an error, since `listRemote`
needs to treat 404 as "no notes yet" rather than a failure. `putFile` uses the
full 3-attempt pattern; `listRemote` uses `maxAttempts=2` (one retry). `commit
21451a6`, with tests against `doWithRetry` directly (`listRemote`/`putFile`
hardcode the `api.github.com` URL, unlike `chat.go`'s injectable `baseURL`, so
they aren't independently server-mockable).

### D8 — `transcribe` buffer not pre-sized (TRIVIAL) ✅ Done

`chat.go:239` — `bytes.Buffer` grows by doubling through ~4.5 MiB of writes.
`buf.Grow(len(audio)+512)` (or `len(audioB64)+512` post-D6) makes it one allocation.

**Fixed:** `buf.Grow(len(audio) + 512)` added right after the buffer is declared
in `transcribe()`. `commit 5724dfb`. Superseded in part by D6, which changes what
`transcribe` receives and re-derives the grow size from the base64 length.

### D9 — `/upload` keeps base64 + decoded bytes both live through the file write (TRIVIAL) ✅ Done

`notessync.go` upload handler: set `req.ContentB64 = ""` immediately after
`DecodeString` so GC can drop the ~1.33× string before `os.WriteFile` runs.

**Fixed:** `req.ContentB64 = ""` added right after the `DecodeString` call
in `upload()`. `commit ae4f83e`.

### D10 — `putFile` `json.Marshal` duplicates the file's base64 once more (LOW)

`notessync.go:267–277` — the marshal copies `contentB64` into a fresh buffer
(~2.66× raw file live per PUT). Fix only if D6/D9 measurements still show pressure:
hand-build the small JSON envelope into a pre-grown `bytes.Buffer` (escape `name`
carefully) — or fold into the D5 Git-Data-API rework, which restructures this path
anyway.

### D11 — Shutdown window 5 s < upstream timeouts (TRIVIAL) ✅ Done

`cmd/lilyhub/main.go:75` — `Shutdown` context is 5 s; notes-sync client timeout is
30 s (`notessync.go:97`), chat ≤60 s. `systemctl stop` mid-sync cancels in-flight
GitHub PUTs and 502s the device. Raise to ~35 s (sync bound + margin); chat's 60 s
can be allowed to die — history is in-memory anyway.

**Fixed:** `Shutdown` context raised from 5s to 35s in `main()`. `commit d3d78f6`.

### D12 — Retry sleep uses `time.After` (LOW, idiomatic fix) ✅ Done

`chat.go:419` — un-stoppable timer lingers ≤2 s per cancelled retry. Swap for
`time.NewTimer` + `Stop()` in the select.

**Fixed — both copies of the pattern.** Re-verified before editing: the same
`select { case <-time.After(d): case <-req.Context().Done(): ... }` shape
exists twice — `chat.go`'s `do()` (:434, the one this entry names) and
`notessync.go`'s `doWithRetry` (:68, D7's deliberate mirror of `chat.go`'s
retry pattern, per its own header comment). Fixed both for consistency: each
now does `timer := time.NewTimer(d)`, selects on `timer.C`, and calls
`timer.Stop()` in the context-done branch before returning. `gofmt -l .` +
`go vet ./...` + `go build ./...` + `go test ./...` all clean/pass across all
six packages. `commit 026a947`.

### D13 — geoSearch relays ~60 % dead payload to the device (LOW, optional) ✅ Done

`internal/weather/weather.go` proxies the open-meteo geocoding response verbatim;
firmware parses only `results[].{name,latitude,longitude,country,admin1}`
(`ui_weather.cpp:614–638`) while elevation/timezone/ids/postcodes ride along (~2–4 KB
per search the ESP32 must cJSON-parse). Fix = dedicated trim handler for this one
route; it breaks the "verbatim proxy" simplicity — only if device-side parse cost
ever matters.

**Fixed — the file:line citation was imprecise, corrected before editing.**
`ui_weather.cpp:614–638` (`fetch_geo_city`, the *only* consumer that goes through
the hub route) requests `count=1` and reads just `results[0].{latitude,longitude}`
— it never reads `name`/`country`/`admin1`. The full `name/latitude/longitude/
country/admin1` field set the original entry named is read by a second consumer,
`weather_search_cities` (`ui_weather.cpp:1247–1303`), which calls
`geocoding-api.open-meteo.com` **directly** (`hw_http_get_string`), bypassing the
hub entirely — so today, nothing actually reads the trimmed fields *through* this
route. Kept the full 5-field set in the trim anyway rather than narrowing to just
lat/lon: `geoSearch`'s own comment ("count…default to 10, matches the device's old
behavior") signals the route was sized for the list-search use case, and trimming
to only what the sole current hub consumer reads would silently break the route
for `weather_search_cities` the day it's pointed at the hub (an obviously-intended
future wire-up, not a hypothetical). Implemented as a `transform` hook added to
`httpx.Proxy` (new optional 9th param, `func([]byte) ([]byte, error)`, nil at the
two other call sites) rather than a fully separate handler — reuses the existing
fetch/size-cap/validate/cache/error-mapping instead of duplicating it, while still
keeping the "verbatim proxy" default path untouched for `geoIP`/`forecast`.
`trimGeoSearch` decodes into a `geoSearchResult{Name,Latitude,Longitude,Country,
Admin1}` struct (via `encoding/json`, ignoring every other upstream field) and
re-marshals; a missing/empty `results` array round-trips as an omitted field,
which the device's `cJSON_IsArray` checks already treat as "no matches" same as
before. `gofmt -l .` + `go vet ./...` + `go build ./...` + `go test ./...` clean
across all packages, with new tests: `httpx` gained
`TestProxyTransformRewritesBodyBeforeCaching`/`TestProxyTransformErrorNotCached`;
`weather` gained `TestTrimGeoSearchDropsUnusedFields` (real open-meteo-shaped
payload in, asserts every dropped field is gone from the bytes and the five kept
fields round-trip exactly) / `TestTrimGeoSearchEmptyResults` /
`TestTrimGeoSearchInvalidJSON`. Live-verified end-to-end: ran `lilyhub` locally
against the real `geocoding-api.open-meteo.com` (`name=Paris&count=3`) — MISS
response came back trimmed to the 5 fields only (confirmed `elevation`,
`country_code`, `timezone`, `population`, `postcodes`, the numeric `id`, etc. are
all absent), and the follow-up HIT served an identical cached body. Firmware
untouched by this change (server-only); `pio run -e tlora_pager` + `pio run -e
emulator_lora_pager` + `pio test -e native_test` re-run per standing build
discipline anyway, all pass (unaffected, as expected). `commit <pending>`.

### D14 — No `Cache-Control` toward the device (LOW, needs firmware change too)

`internal/httpx/httpx.go` — hub TTLs and the device's NVS `WEATHER_FRESH_TTL_SEC`
are independently hardcoded. Additive `max-age` header would let the device align.
Park until a firmware change wants it.

---

## E. Carried forward — still-open items from phases 1–2 (do not lose)

| Item | Where documented | State |
|---|---|---|
| **Hardware smoke-test checklist** (radio/audio/§2.13/§2.17/P2.1/P2.2/P2.8) | `OPTIMIZATION_PROGRESS.md` | ☐ blocks trusting those passes; extend with P3.7–P3.12 verifications |
| P2.5 GPS rail latched 24/7 when enabled | PHASE2 §A | ☐ product decision |
| P2.6 `playerTask` 8 KB boot-time stack | PHASE2 §A | ☐ pair with P3.10 |
| P2.9 Journal reconcile on LVGL thread | PHASE2 §B | ☐ only if post-sync jank observed |
| P2.10(B) notes-sync full upload body in device RAM | PHASE2 §B | ☐ pair with D5 |
| P2.11 font product decisions (picker cap −75 KB, mono face −28–31 KB, emoji −43 KB) | PHASE2 §C | ☐ user call |
| P2.12 WiFi power-save on fake sleep | PHASE2 §C | ☐ product decision |
| §4 D5 Git Data API (batch commits, unblocks parallelism) + A1/A2 cosmetics | PHASE2 §D | ☐ |
| §1.5 write-only `monitor_params_t` fields (removes live I2C reads) | PROGRESS Deferred | ☐ hardware pass |
| §2.16 `hw_http_request` double-buffer | PROGRESS Deferred | ⊘ stays deferred |
| §1.7 timezone-extern layering fix | PROGRESS Deferred | ☐ blocked on relocation |
| lib history rewrite for the untracked 96 MB firmware bins | PROGRESS §3.5 | ⊘ disruptive, optional |

---

## Verified clean this pass (don't re-sweep)

- **Apps:** all settings subpages, menu_app/menu_glance, weather, chat (HTTP paths),
  text editor, media remote, list picker, wifi UI, core/ — no further LVGL-thread
  blocking beyond P3.1–P3.6. `settings_info` 1 Hz tick is page-gated; storage bulk
  ops throttle + yield correctly.
- **HAL:** monitor TTL caching, device-online bitmask, IMU batch config, LVGL tick/
  flush config, boot ordering (LVGL before connectivity), CPU-freq policy, WiFi
  auto-reconnect choice — all sound. All big audio/NFC/LVGL buffers already PSRAM.
- **Server:** client reuse, contexts, body closes, cache error-isolation, chat
  mutex-release-before-network, canonical cache keys, graceful-shutdown wiring
  (modulo D11's window), gofmt/vet clean.
- **Build:** montserrat set minimal, generated fonts all live, RadioLib excludes
  correct, LVGL log/profiler/demos off, cache sizes zeroed, Makefile/tools clean.

---

## Suggested execution order

1. **Bug fixes first, tiny and safe:** P3.8 ✅ (tg_bg stack — one number), P3.11 ✅
   (hub_probe guard), P3.22 ✅ (board variant field), P3.23 ✅ (emulator font drift).
   One commit each.
2. ✅ **Server quick wins (independent codebase):** D8, D9, D11 (trivial); then D6
   (streaming decode) + D7 (retry) with tests, mirroring the D1–D4 commit style;
   D12 ✅ (retry-sleep timer leak — fixed in both `chat.go` and `notessync.go`'s
   mirrored copy of the pattern). D10, D13, D14 remain deliberately
   deferred/optional (see their entries).
3. ✅ **UI-thread stalls:** P3.2 ✅ (notes-sync log — small), P3.6 ✅ (chat mkdir), P3.1 ✅
   (tasks debounce), P3.5 ✅ (telegram sanitize-at-parse), P3.3 ✅ (audio-notes
   worker+drain), P3.4 ✅ (SSH trim — done, deviated from the literal fix; see its
   entry). Step complete.
4. ✅ **lv_conf/flash batch:** P3.17 + P3.21 together ✅ (arc stayed enabled — spinner
   dependency; see P3.17's entry), P3.18 ✅ (0 B measured — see its entry for why
   the estimate was wrong), P3.20 ✅ (−13.5 KB measured, landed in `env_arduino`
   — see its entry), P3.19 ✅ (−18.8 KB measured — see its entry; also surfaced an
   unrelated pre-existing crash bug in the in-flight `ui_settings.cpp` keyboard-
   shortcut diff, flagged to the user, not fixed here). Step complete.
5. **Internal-RAM levers with measurement:** P3.15 ✅, P3.16 ✅ (both pure code, no
   HW dependency). P3.7 (watermark → shrink loopTask) and P3.14 (ble_kb_ka watermark)
   re-confirmed blocked 2026-07-14 — no LilyGo device attached to this machine
   (`pio device list`/`lsusb` checked); do not guess the numbers. Picked up the two
   remaining pure-code, zero/very-low-risk, no-HW-to-*land* items instead: P3.13 ✅
   (rotary fake-sleep notify-block — verification of wake latency itself still
   wants hardware, added to the smoke-test checklist, but the code change needed
   none to write or build) and P3.26 ✅ (test_desktop → str_encode round-trip
   suite — pure test infra, zero risk). Re-confirmed again same session
   (2026-07-14): still no device attached (checked a third time this same day,
   in a follow-up session — `pio device list`/`lsusb` unchanged). Picked up
   D12 (§2, above) as another pure-code, zero-HW item while still blocked.
   Re-confirmed a fourth time (2026-07-14, follow-up session): still no device
   attached (`pio device list`/`lsusb` unchanged). Picked up D13 (§D, above —
   geoSearch trim) as another pure-code, zero-HW, server-only item; its
   file:line citation didn't survive re-verification (see its entry) but the
   fix itself landed clean with a live end-to-end check against real
   open-meteo. D14 stays parked — it explicitly needs a firmware-side change
   that doesn't exist yet, unlike D13. **Next up: a hardware session**
   (step 6) — it now gates P3.7/P3.9/P3.10/P3.12/P3.14/P3.24 all at once, so
   batch them into one bench pass rather than trickling in. If no hardware
   session materializes, D14 and the P2.11 font product decisions (step 7)
   are the only remaining work in this doc.
6. **Hardware session:** run the full smoke-test checklist (phases 1–2 backlog +
   P3.7/9/10/12/24 verifications), then the P3.12 SD-rail bench and the P3.25 Klio
   investigation on a sacrificial device.
7. **Product decisions** (P2.5/P2.11/P2.12, P3.27) whenever the user weighs in.

Per repo convention: one `<code>` commit + one `<docs>` commit per item; update this
file's findings with a ✅/status note as they land; keep
`OPTIMIZATION_PROGRESS.md`'s methodology in force — **re-verify every file:line here
against current source before editing. This file will go stale the same way the
phase-1 and phase-2 docs did.**
