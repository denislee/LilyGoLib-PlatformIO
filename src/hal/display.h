/**
 * @file      display.h
 * @brief     Display backlight, brightness, keyboard input.
 */
#pragma once

#include "types.h"

void hw_set_disp_backlight(uint8_t level);
uint8_t hw_get_disp_backlight();
bool hw_get_disp_is_on();
const uint32_t hw_get_disp_timeout_ms();

void hw_inc_brightness(uint8_t level);
void hw_dec_brightness(uint8_t level);
uint8_t hw_get_disp_min_brightness();
uint16_t hw_get_disp_max_brightness();

void hw_set_kb_backlight(uint8_t level);
uint8_t hw_get_kb_backlight();
void hw_set_led_backlight(uint8_t level);
bool hw_has_indicator_led();

void hw_enable_keyboard();
void hw_disable_keyboard();
void hw_flush_keyboard();
bool hw_has_keyboard();
void hw_set_keyboard_read_callback(void(*read)(int state, char &c));

bool is_screen_small();
