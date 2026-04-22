/**
 * @file      keyboard_task.cpp
 * @brief     High-priority keyboard reader task + LVGL indev bridge.
 *
 * Why this exists: the stock LVGL keypad read_cb (in LilyGoLib's
 * LV_Helper_v9.cpp) calls `instance.kb.getKey()` synchronously from the
 * LVGL task. Any long operation holding the instance mutex — WiFi scan,
 * NFC discovery, heavy redraw, audio init — would delay or drop key
 * presses. Users saw "keyboard stops working" symptoms.
 *
 * Here we spin up a dedicated FreeRTOS task at the highest non-system
 * priority, pinned to core 0 (opposite of the Arduino loop on core 1),
 * that drains the TCA8418 every ~10 ms and pushes decoded key events
 * into a queue. We then replace the keypad indev's read_cb with one that
 * only drains the queue — zero I2C work on the LVGL task.
 *
 * Lock behaviour: reads still go through the shared `instance` mutex, but
 * (a) we hold it only for the duration of a getKey() burst and (b) the
 * mutex has priority inheritance (xSemaphoreCreateMutex), so if a
 * low-priority holder is running while we block, it gets boosted to our
 * priority and releases sooner.
 */
#include "keyboard_task.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "display.h"
#include "system.h"
#include "../core/scoped_lock.h"

#ifdef USING_INPUT_DEV_KEYBOARD

namespace {

struct KeyEvent {
    int  state;  // KB_PRESSED / KB_RELEASED
    char c;
};

constexpr UBaseType_t kTaskPriority = configMAX_PRIORITIES - 2;
constexpr BaseType_t  kTaskCore     = 0;   // Arduino loop runs on core 1
constexpr uint32_t    kPollMs       = 10;  // Faster than the 100 ms fallback in LilyGoKeyboard.
constexpr UBaseType_t kQueueDepth   = 32;

QueueHandle_t s_event_queue = nullptr;
TaskHandle_t  s_task        = nullptr;
uint32_t      s_last_key    = 0;

void keyboard_task_fn(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));

        if ((hw_get_device_online() & HW_KEYBOARD_ONLINE) == 0) {
            continue;
        }

        // Drain everything pending in a single lock hold so we don't
        // thrash the mutex on bursts (fast typing can emit several events
        // per poll).
        core::ScopedInstanceLock lock;
        for (int i = 0; i < 16; ++i) {
            char c = '\0';
            int state = instance.kb.getKey(&c);
            if (state != KB_PRESSED && state != KB_RELEASED) {
                break;
            }
            KeyEvent ev{state, c};
            // Drop oldest if the UI somehow falls behind 32 events —
            // better to lose a stale key than to stall the reader.
            if (xQueueSend(s_event_queue, &ev, 0) != pdTRUE) {
                KeyEvent discard;
                xQueueReceive(s_event_queue, &discard, 0);
                xQueueSend(s_event_queue, &ev, 0);
            }
        }
    }
}

uint32_t get_byte_pos(const char *txt, uint32_t char_pos)
{
    uint32_t bp = 0;
    while (char_pos > 0 && txt[bp] != '\0') {
        uint8_t c = txt[bp];
        if      ((c & 0x80) == 0x00) bp += 1;
        else if ((c & 0xE0) == 0xC0) bp += 2;
        else if ((c & 0xF0) == 0xE0) bp += 3;
        else if ((c & 0xF8) == 0xF0) bp += 4;
        else                         bp += 1;
        char_pos--;
    }
    return bp;
}

// Handle Alt+Backspace word delete inline (same behaviour as the stock
// LVGL keypad_read in LV_Helper_v9.cpp).
void handle_alt_backspace(lv_indev_t *drv)
{
    lv_group_t *g = lv_indev_get_group(drv);
    if (!g) return;
    lv_obj_t *focused = lv_group_get_focused(g);
    if (!focused || !lv_obj_has_class(focused, &lv_textarea_class)) return;

    bool deleting_spaces = true;
    while (true) {
        const char *txt = lv_textarea_get_text(focused);
        uint32_t cursor_pos = lv_textarea_get_cursor_pos(focused);
        if (cursor_pos == 0 || !txt) break;
        uint32_t bp = get_byte_pos(txt, cursor_pos - 1);
        bool is_space = (txt[bp] == ' ');
        if (deleting_spaces) {
            if (!is_space) deleting_spaces = false;
        } else {
            if (is_space) break;
        }
        lv_textarea_delete_char(focused);
    }
}

void kb_read_cb(lv_indev_t *drv, lv_indev_data_t *data)
{
    KeyEvent ev;
    if (!s_event_queue || xQueueReceive(s_event_queue, &ev, 0) != pdTRUE) {
        data->state = LV_INDEV_STATE_REL;
        data->key   = s_last_key;
        return;
    }

    bool more = uxQueueMessagesWaiting(s_event_queue) > 0;

    if (ev.state == KB_PRESSED) {
        if (ev.c == 0x17) {  // Alt+Backspace
            handle_alt_backspace(drv);
            data->state = LV_INDEV_STATE_REL;
            data->continue_reading = more;
            return;
        }
        s_last_key = (uint32_t)(uint8_t)ev.c;
        data->key   = s_last_key;
        data->state = LV_INDEV_STATE_PR;
        data->continue_reading = more;
        return;
    }

    // Released
    data->state = LV_INDEV_STATE_REL;
    data->key   = ev.c ? (uint32_t)(uint8_t)ev.c : s_last_key;
    data->continue_reading = more;
}

}  // namespace

void hw_keyboard_task_start()
{
    if (s_task) return;
    if (!(hw_get_device_online() & HW_KEYBOARD_ONLINE)) return;

    s_event_queue = xQueueCreate(kQueueDepth, sizeof(KeyEvent));
    if (!s_event_queue) {
        log_e("kb_reader: queue alloc failed");
        return;
    }

    // Steal LVGL's keypad read_cb so LVGL drains our queue instead of
    // hitting I2C on its own task.
    if (lv_indev_t *kb = lv_get_keyboard_indev()) {
        lv_indev_set_read_cb(kb, kb_read_cb);
    } else {
        log_w("kb_reader: no LVGL keypad indev found; polling will still run");
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        keyboard_task_fn, "kb_reader", 4096, nullptr,
        kTaskPriority, &s_task, kTaskCore);
    if (ok != pdPASS) {
        log_e("kb_reader: task create failed");
        s_task = nullptr;
    }
}

#else  // !USING_INPUT_DEV_KEYBOARD

void hw_keyboard_task_start() {}

#endif  // USING_INPUT_DEV_KEYBOARD

#else  // !ARDUINO

void hw_keyboard_task_start() {}

#endif  // ARDUINO
