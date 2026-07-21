/**
 * @file      nfc_task.cpp
 * @brief     Dedicated NFC polling task — keeps the ST25R3916 discovery
 *            state machine off the main LVGL loop.
 *
 * Previously `loopNFCReader()` ran inside the `loop()`-wide
 * `ScopedInstanceLock` in factory.ino, so any blocking SPI transaction
 * in the RFAL state machine directly delayed `lv_timer_handler()`. Now
 * it runs on its own core-0 task (opposite the Arduino loopTask / LVGL
 * task on core 1) and holds the instance mutex only for the duration
 * of a single poll, with priority inheritance ensuring a blocked
 * high-priority waiter boosts whoever is holding the lock.
 */
#include "nfc_task.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../core/scoped_lock.h"
#include "../core/system_hooks.h"
#include "nfc_reader.h"
#include "peripherals.h"

#if defined(USING_ST25R3916)

namespace {

constexpr UBaseType_t kTaskPriority = configMAX_PRIORITIES - 3;
constexpr BaseType_t  kTaskCore     = 0;
constexpr uint32_t    kPollMs       = 100;  // ~10 Hz — 5× cut in instance-lock traffic vs. the old 50 Hz; tap latency unaffected (RFAL responds as soon as a tag is detected, not on this timer boundary)
// Fallback cadence while NFC is off or the display is in fake-sleep.
// hw_start_nfc_discovery() kicks hw_nfc_task_notify_wake() when the user
// enables NFC, so enable-latency is unaffected. ui_resume_timers() does NOT
// kick the NFC task, so raising this to 1 s adds up to ~800 ms latency to
// NFC scan resumption after fake-sleep wake (acceptable — not latency-critical).
constexpr uint32_t    kIdleMs       = 1000;

TaskHandle_t s_task = nullptr;

void nfc_task_fn(void *)
{
    for (;;) {
        // NFC discovery is off by default. When it isn't running — or the
        // display is in fake-sleep, where hw_power_down_all() cuts the NFC
        // rail — loopNFCReader() would only early-return on !_nfc_running, so
        // taking the instance mutex 50×/s to reach that return is pure
        // contention with the keyboard/LVGL tasks. Block on a task notify
        // instead; hw_start_nfc_discovery() kicks us via
        // hw_nfc_task_notify_wake(), and the timeout bounds latency if a
        // notify is ever missed (e.g. discovery already active on wake).
        if (ui_is_fake_sleep() || !hw_nfc_discovery_active()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleMs));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(kPollMs));

        core::ScopedInstanceLock lock;
        loopNFCReader();
    }
}

}  // namespace

void hw_nfc_task_notify_wake()
{
    // Safe from any task context; a give while the task is running (not
    // blocked) simply leaves the notification pending for the next take.
    if (s_task) xTaskNotifyGive(s_task);
}

void hw_nfc_task_start()
{
    if (s_task) return;

    BaseType_t ok = xTaskCreatePinnedToCore(
        nfc_task_fn, "nfc_reader", 4096, nullptr,
        kTaskPriority, &s_task, kTaskCore);
    if (ok != pdPASS) {
        log_e("nfc_task: task create failed");
        s_task = nullptr;
    }
}

#else  // !USING_ST25R3916

void hw_nfc_task_start() {}
void hw_nfc_task_notify_wake() {}

#endif  // USING_ST25R3916

#else  // !ARDUINO

void hw_nfc_task_start() {}
void hw_nfc_task_notify_wake() {}

#endif  // ARDUINO
