/**
 * @file      radio.cpp
 * @brief     Radio enable/disable.
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

int16_t hw_set_radio_enable(bool en)
{
    user_setting.radio_enable = en;
    if (en) {
        return hw_set_radio_default();
    }
    radio_params_t params;
    hw_get_radio_params(params);
    params.mode = RADIO_DISABLE;
    return hw_set_radio_params(params);
}
