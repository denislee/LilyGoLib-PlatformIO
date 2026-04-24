/**
 * @file      rotary_task.h
 * @brief     Dedicated rotary-encoder reader task + LVGL indev bridge.
 *
 * Mirror of keyboard_task: moves the blocking `instance.getRotary()` call
 * off the LVGL task so a long mutex hold elsewhere can no longer stutter
 * the scroll wheel. Must be started AFTER `beginLvglHelper()` so the
 * LVGL encoder indev exists and our read_cb can be swapped in.
 */
#pragma once

void hw_rotary_task_start();
