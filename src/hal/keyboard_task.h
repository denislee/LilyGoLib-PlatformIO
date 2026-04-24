/**
 * @file      keyboard_task.h
 * @brief     High-priority keyboard reader task.
 *
 * Decouples keyboard scanning from the main LVGL loop so key presses are
 * never dropped when the main task is busy (display flushes, NFC reader,
 * WiFi/BLE, audio). A dedicated FreeRTOS task polls the TCA8418 on a tight
 * schedule at higher-than-app priority and funnels events through a queue
 * that LVGL drains non-blocking.
 */
#pragma once

void hw_keyboard_task_start();

// Drop all queued press events and any further events until the currently
// held key is physically released. Used after a navigation action (back
// button, menu switch) so a still-held Enter key doesn't auto-repeat into
// the newly-focused widget on the next screen.
void hw_keyboard_drop_until_release();
