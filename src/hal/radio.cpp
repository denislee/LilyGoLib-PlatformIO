/**
 * @file      radio.cpp
 * @brief     Radio enable/disable + USB/RF switch.
 *
 * The per-module driver implementations (sx1262, cc1101, sx1280, lr1121, nrf24)
 * live in src/hw_*.cpp.
 */
#include "radio.h"
#include "internal.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

bool hw_get_radio_enable() { return user_setting.radio_enable; }

void hw_set_radio_enable(bool en)
{
    user_setting.radio_enable = en;
    if (en) {
        hw_set_radio_default();
    } else {
        radio_params_t params;
        hw_get_radio_params(params);
        params.mode = RADIO_DISABLE;
        hw_set_radio_params(params);
    }
}

void hw_set_usb_rf_switch(bool to_usb)
{
#ifdef ARDUINO
#if defined(HAS_USB_RF_SWITCH)
    instance.setRFSwitch(to_usb);
#endif
#endif
}
