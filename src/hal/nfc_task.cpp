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

#if defined(USING_ST25R3916)

namespace {

constexpr UBaseType_t kTaskPriority = configMAX_PRIORITIES - 3;
constexpr BaseType_t  kTaskCore     = 0;
constexpr uint32_t    kPollMs       = 20;

TaskHandle_t s_task = nullptr;

void nfc_task_fn(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));

        if (ui_is_fake_sleep()) continue;

        core::ScopedInstanceLock lock;
        loopNFCReader();
    }
}

}  // namespace

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

#endif  // USING_ST25R3916

#else  // !ARDUINO

void hw_nfc_task_start() {}

#endif  // ARDUINO
