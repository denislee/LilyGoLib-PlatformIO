/**
 * @file      lvgl_task.cpp
 * @brief     Dedicated LVGL handler task.
 *
 * Running `lv_timer_handler()` from the Arduino `loop()` meant every
 * tick paid the cost of whatever else shared that loop (vendor
 * instance.loop, NFC polling, CPU scaling reads). Here we give LVGL
 * its own core-1 task so render and input-processing cadence is
 * determined by LVGL itself, not by an unrelated loop body.
 *
 * The task calls `core::System::getInstance().loop()` alongside LVGL
 * because both drive UI-side state and must see each tick together.
 */
#include "lvgl_task.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../core/scoped_lock.h"
#include "../core/system.h"
#include "../core/system_hooks.h"

namespace {

// Priority sits above the Arduino loopTask (1) but well below WiFi/BLE system
// tasks so we don't block radio work. Running at configMAX_PRIORITIES - 2 meant
// a long app-side operation (journal scan / AES decrypt) could starve IDLE1 on
// core 1, which the task watchdog catches as a panic → reboot. 8 is generous
// for UI responsiveness without the starvation hazard.
constexpr UBaseType_t kTaskPriority = 8;
constexpr BaseType_t  kTaskCore     = 1;
constexpr uint32_t    kIdleMs       = 50;
constexpr uint32_t    kMaxTickMs    = 5;
// FFat reads plus mbedTLS AES-CBC decrypt plus nested LVGL event dispatch
// (e.g. menu rebuild from a click handler) easily cleared 6KB on 8KB stacks.
constexpr uint32_t    kStackBytes   = 16384;

TaskHandle_t s_task = nullptr;

void lvgl_task_fn(void *)
{
    for (;;) {
        if (ui_is_fake_sleep()) {
            vTaskDelay(pdMS_TO_TICKS(kIdleMs));
            continue;
        }

        uint32_t next;
        {
            core::ScopedInstanceLock lock;
            next = lv_timer_handler();
            core::System::getInstance().loop();
        }

        if (next > kMaxTickMs) next = kMaxTickMs;
        if (next == 0)         next = 1;
        vTaskDelay(pdMS_TO_TICKS(next));
    }
}

}  // namespace

void hw_lvgl_task_start()
{
    if (s_task) return;

    BaseType_t ok = xTaskCreatePinnedToCore(
        lvgl_task_fn, "lvgl", kStackBytes, nullptr,
        kTaskPriority, &s_task, kTaskCore);
    if (ok != pdPASS) {
        log_e("lvgl_task: task create failed");
        s_task = nullptr;
    }
}

#else  // !ARDUINO

void hw_lvgl_task_start() {}

#endif  // ARDUINO
