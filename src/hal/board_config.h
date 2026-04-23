/**
 * @file      board_config.h
 * @brief     Per-board compile-time configuration (capabilities, fonts, tips).
 *
 * Activated by the ARDUINO_T_* defines set by PlatformIO board variants.
 */
#pragma once

#if defined(ARDUINO_T_LORA_PAGER)

#define USING_BLE_KEYBOARD
#define FLOAT_BUTTON_WIDTH   40
#define FLOAT_BUTTON_HEIGHT  40

#ifndef RADIOLIB_EXCLUDE_NRF24
#define USING_EXTERN_NRF2401
#endif
/* USING_ST25R3916 is injected as a -D build flag in platformio.ini so it
 * reaches every translation unit (incl. app_nfc.cpp which doesn't pull in
 * this header). */

#define MAIN_FONT   &lv_font_montserrat_20
#define NFC_TIPS_STRING "Place the NFC card close to the center of the arrow on the back. It will vibrate when the card is detected; otherwise, it will not display anything if it cannot be resolved."
#define DEVICE_KEYBOARD_TYPE    KEYBOARD_TYPE_1

#elif defined(ARDUINO_T_WATCH_S3_ULTRA)

#define USING_TOUCHPAD
#define FLOAT_BUTTON_WIDTH   60
#define FLOAT_BUTTON_HEIGHT  60
#define USING_BLE_KEYBOARD

#ifndef USING_BHI260_SENSOR
#define USING_BHI260_SENSOR
#endif
#ifndef USING_ST25R3916
#define USING_ST25R3916
#endif
#ifndef HAS_USB_RF_SWITCH
#define HAS_USB_RF_SWITCH
#endif

#define NFC_TIPS_STRING "Hold the NFC card close to the front of the screen. It will vibrate when the card is detected; otherwise, it will not display anything if it cannot be resolved."
#define MAIN_FONT   &lv_font_montserrat_22

#elif defined(ARDUINO_T_WATCH_S3)

#define USING_TOUCHPAD
#define FLOAT_BUTTON_WIDTH   40
#define FLOAT_BUTTON_HEIGHT  40

#ifndef USING_BMA423_SENSOR
#define USING_BMA423_SENSOR
#define USING_BLE_KEYBOARD
#endif

#define NFC_TIPS_STRING "No NFC devices"
#define MAIN_FONT   &lv_font_montserrat_16

#endif
