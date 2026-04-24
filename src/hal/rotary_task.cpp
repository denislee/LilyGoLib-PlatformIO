/**
 * @file      rotary_task.cpp
 * @brief     High-priority rotary reader task + LVGL indev bridge.
 *
 * Why this exists: the stock LVGL encoder read_cb (LV_Helper_v9.cpp's
 * `lv_encoder_read`) calls `instance.getRotary()` from the LVGL task.
 * That call blocks up to 50 ms on the vendor's rotary queue, and it
 * runs while the LVGL task holds the instance mutex. So:
 *   - Every idle indev poll pays a 50 ms wait under the mutex, and
 *   - If anything else takes the instance mutex for ~1 s (WiFi, NFC,
 *     FFat, radio), the LVGL task blocks entirely and scroll events
 *     stop reaching LVGL — the symptom users see is "scroll wheel
 *     stutters / freezes for 1–2 s, then catches up."
 *
 * Fix (same pattern as keyboard_task): spin up a dedicated task that
 * drains `instance.getRotary()` — a pure xQueueReceive that does NOT
 * need the instance mutex — into our own queue, and replace the LVGL
 * encoder indev's read_cb with one that pops non-blockingly. While the
 * LVGL task is blocked on the mutex, our reader keeps filling the
 * queue; as soon as LVGL runs again, it drains everything in one
 * timer cycle (via `continue_reading`).
 */
#include "rotary_task.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#ifdef USING_INPUT_DEV_ROTARY

namespace {

constexpr UBaseType_t kTaskPriority = configMAX_PRIORITIES - 2;
constexpr BaseType_t  kTaskCore     = 0;   // Opposite of Arduino loopTask / LVGL task (core 1).
constexpr UBaseType_t kQueueDepth   = 16;  // Burst of fast clicks while LVGL is briefly blocked.

QueueHandle_t s_event_queue = nullptr;
TaskHandle_t  s_task        = nullptr;

void enqueue_event(const RotaryMsg_t &ev)
{
    // Drop the oldest pending event when full — better to lose a stale
    // click than to stall the reader (would back-pressure onto the
    // vendor rotaryTask, which sends with portMAX_DELAY).
    if (xQueueSend(s_event_queue, &ev, 0) != pdTRUE) {
        RotaryMsg_t discard;
        xQueueReceive(s_event_queue, &discard, 0);
        xQueueSend(s_event_queue, &ev, 0);
    }
}

void rotary_task_fn(void *)
{
    for (;;) {
        // instance.getRotary() blocks up to 50 ms on the vendor's
        // rotaryMsg queue. That's fine here — this task has no other
        // job and does not hold the instance mutex. On real scroll
        // activity we return immediately with an event; on idle we
        // just loop every 50 ms.
        RotaryMsg_t msg = instance.getRotary();
        if (msg.dir != ROTARY_DIR_NONE || msg.centerBtnPressed) {
            enqueue_event(msg);
        }
    }
}

bool is_word_boundary_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
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

// Mirror of LV_Helper_v9.cpp::textarea_jump_word so Alt+scroll still
// hops by word when our read_cb is in charge.
void textarea_jump_word(lv_obj_t *ta, int direction)
{
    const char *txt = lv_textarea_get_text(ta);
    if (!txt) return;

    if (direction > 0) {
        uint32_t len = lv_strlen(txt);
        bool skipped_word = false;
        while (true) {
            uint32_t pos = lv_textarea_get_cursor_pos(ta);
            if (pos >= len) break;
            uint32_t bp = get_byte_pos(txt, pos);
            bool is_space = is_word_boundary_char(txt[bp]);
            if (!skipped_word) {
                if (!is_space) skipped_word = true;
            } else {
                if (is_space) break;
            }
            lv_textarea_cursor_right(ta);
        }
    } else {
        bool skipped_word = false;
        while (true) {
            uint32_t pos = lv_textarea_get_cursor_pos(ta);
            if (pos == 0) break;
            uint32_t bp = get_byte_pos(txt, pos - 1);
            bool is_space = is_word_boundary_char(txt[bp]);
            if (!skipped_word) {
                if (!is_space) skipped_word = true;
            } else {
                if (is_space) break;
            }
            lv_textarea_cursor_left(ta);
        }
    }
}

void rotary_read_cb(lv_indev_t *drv, lv_indev_data_t *data)
{
    static uint8_t last_dir = ROTARY_DIR_NONE;

    // Coalesce every queued rotary event into one aggregate enc_diff. A
    // fast flick emits many notches in quick succession; feeding them one
    // at a time forces a full LVGL scroll+flush per notch, which shows up
    // as stutter when the flush contends with the instance mutex. Summing
    // lets a single render cycle cover the whole flick.
    int     diff_total     = 0;
    bool    center_pressed = false;
    uint8_t latest_dir     = ROTARY_DIR_NONE;
    bool    any_event      = false;

    while (s_event_queue) {
        RotaryMsg_t msg;
        if (xQueueReceive(s_event_queue, &msg, 0) != pdTRUE) break;
        any_event = true;
        switch (msg.dir) {
        case ROTARY_DIR_UP:   diff_total += 1; break;
        case ROTARY_DIR_DOWN: diff_total -= 1; break;
        default: break;
        }
        if (msg.dir != ROTARY_DIR_NONE) latest_dir = msg.dir;
        if (msg.centerBtnPressed) center_pressed = true;
    }

    if (!any_event) {
        data->enc_diff = 0;
        data->state    = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Alt+scroll jumps the textarea cursor by word. Matches vendor read_cb,
    // scaled by the aggregate so a 3-notch flick hops 3 words.
    if (diff_total != 0 && instance.isAltKeyPressed()) {
        lv_group_t *g = lv_indev_get_group(drv);
        if (g && lv_group_get_editing(g)) {
            lv_obj_t *focused = lv_group_get_focused(g);
            if (focused && lv_obj_has_class(focused, &lv_textarea_class)) {
                int dir   = diff_total > 0 ? 1 : -1;
                int steps = diff_total > 0 ? diff_total : -diff_total;
                for (int i = 0; i < steps; ++i) {
                    textarea_jump_word(focused, dir);
                }
                data->enc_diff = 0;
                data->state    = center_pressed ? LV_INDEV_STATE_PRESSED
                                                : LV_INDEV_STATE_RELEASED;
                if (last_dir != latest_dir || center_pressed) {
                    instance.feedback((void *)drv);
                }
                last_dir = latest_dir;
                data->continue_reading =
                    s_event_queue && uxQueueMessagesWaiting(s_event_queue) > 0;
                return;
            }
        }
    }

    data->enc_diff = diff_total;
    data->state    = center_pressed ? LV_INDEV_STATE_PRESSED
                                    : LV_INDEV_STATE_RELEASED;

    if (last_dir != latest_dir || center_pressed) {
        instance.feedback((void *)drv);
    }
    last_dir = latest_dir;

    // Any events that raced in after our drain will be picked up on the
    // next cb call; normally the drain loop above leaves the queue empty.
    data->continue_reading =
        s_event_queue && uxQueueMessagesWaiting(s_event_queue) > 0;
}

}  // namespace

void hw_rotary_task_start()
{
    if (s_task) return;
    if (!instance.hasEncoder()) return;

    s_event_queue = xQueueCreate(kQueueDepth, sizeof(RotaryMsg_t));
    if (!s_event_queue) {
        log_e("rotary_task: queue alloc failed");
        return;
    }

    // Steal LVGL's encoder read_cb so LVGL drains our queue instead of
    // blocking on instance.getRotary() under the instance mutex.
    if (lv_indev_t *enc = lv_get_encoder_indev()) {
        lv_indev_set_read_cb(enc, rotary_read_cb);
    } else {
        log_w("rotary_task: no LVGL encoder indev found; polling will still run");
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        rotary_task_fn, "rotary_reader", 3072, nullptr,
        kTaskPriority, &s_task, kTaskCore);
    if (ok != pdPASS) {
        log_e("rotary_task: task create failed");
        s_task = nullptr;
    }
}

#else  // !USING_INPUT_DEV_ROTARY

void hw_rotary_task_start() {}

#endif  // USING_INPUT_DEV_ROTARY

#else  // !ARDUINO

void hw_rotary_task_start() {}

#endif  // ARDUINO
