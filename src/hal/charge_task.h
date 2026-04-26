/**
 * @file      charge_task.h
 * @brief     Polls VBUS while in fake-sleep so plugging the cable in can
 *            briefly wake the screen and show a charging indicator.
 */
#pragma once

void hw_charge_task_start();
