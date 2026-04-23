/**
 * @file      lvgl_task.h
 * @brief     Dedicated LVGL handler task pinned to core 1.
 *
 * Moves `lv_timer_handler()` and the `core::System` tick out of
 * `loop()` onto a pinned high-priority FreeRTOS task, so the Arduino
 * loopTask can't be blocked by whatever else ends up sharing its
 * cadence. Must be started AFTER `beginLvglHelper()` and after
 * `core::System::getInstance().init()`.
 */
#pragma once

void hw_lvgl_task_start();
