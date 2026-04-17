/**
 * @file      display.cpp
 * @brief     Display backlight, brightness, keyboard input.
 */
#include "display.h"
#include "system.h"
#include "internal.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

const uint32_t hw_get_disp_timeout_ms()
{
    if (user_setting.disp_timeout_second == 0) {
        return 0xFFFFFFF0; // Nearly max, but avoid UINT32_MAX exactly
    }
    return user_setting.disp_timeout_second * 1000UL;
}

void hw_set_disp_backlight(uint8_t level)
{
#ifdef ARDUINO
    instance.setBrightness(level);
#endif
}

uint8_t hw_get_disp_backlight()
{
#ifdef ARDUINO
    return instance.getBrightness();
#else
    return 100;
#endif
}

bool hw_get_disp_is_on()
{
#ifdef ARDUINO
    return instance.getBrightness() != 0;
#else
    return true;
#endif
}

void hw_set_kb_backlight(uint8_t level)
{
#if defined(ARDUINO) && defined(USING_INPUT_DEV_KEYBOARD)
    instance.kb.setBrightness(level);
#endif
}

void hw_set_led_backlight(uint8_t level)
{
#if defined(ARDUINO) && defined(USING_LED_INDICATOR)
    instance.setLedIndicatorBrightness(level);
#endif
}

uint8_t hw_get_kb_backlight()
{
#if defined(ARDUINO) && defined(USING_INPUT_DEV_KEYBOARD)
    return instance.kb.getBrightness();
#else
    return 100;
#endif
}

void hw_inc_brightness(uint8_t level)
{
#ifdef ARDUINO
    instance.incrementalBrightness(level);
#endif
}

void hw_dec_brightness(uint8_t level)
{
#ifdef ARDUINO
    instance.decrementBrightness(level);
#endif
}

uint8_t hw_get_disp_min_brightness()
{
    return dev_conts_var.min_brightness;
}

uint16_t hw_get_disp_max_brightness()
{
    return dev_conts_var.max_brightness;
}

void hw_enable_keyboard()
{
#if defined(ARDUINO)
#if defined(ARDUINO_T_DECK_V2)
    instance.enableKeyboard();
#elif defined(ARDUINO_T_LORA_PAGER)
    instance.initKeyboard();
#endif
    // Ensure user setting is restored as initKeyboard might reset it to library defaults
    hw_set_kb_backlight(user_setting.keyboard_bl_level);
#endif
}

void hw_disable_keyboard()
{
#if defined(ARDUINO)
#if defined(ARDUINO_T_DECK_V2)
    instance.disableKeyboard();
#elif defined(ARDUINO_T_LORA_PAGER)
    instance.kb.end();
#endif
#endif
}

void hw_flush_keyboard()
{
#if defined(ARDUINO) && defined(USING_INPUT_DEV_KEYBOARD)
    if (hw_get_device_online() & HW_KEYBOARD_ONLINE) {
        instance.kb.flush();
    }
#endif
}

bool hw_has_keyboard()
{
    return hw_get_device_online() & HW_KEYBOARD_ONLINE;
}

bool hw_has_indicator_led()
{
    return hw_get_device_online() & HW_LED_INDIC_ONLINE;
}

void hw_set_keyboard_read_callback(void(*read)(int state, char &c))
{
#if defined(ARDUINO) && defined(USING_INPUT_DEV_KEYBOARD)
    instance.kb.setCallback(read);
#endif
}
