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
