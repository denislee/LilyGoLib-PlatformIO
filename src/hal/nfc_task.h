/**
 * @file      nfc_task.h
 * @brief     Dedicated NFC polling task.
 *
 * Decouples ST25R3916 discovery polling from the main loop so NFC SPI
 * operations no longer extend the instance mutex hold window that LVGL
 * needs for rendering. Mirrors the keyboard_task pattern: a pinned
 * FreeRTOS task that briefly grabs `ScopedInstanceLock`, runs
 * `loopNFCReader()`, then sleeps.
 */
#pragma once

void hw_nfc_task_start();
