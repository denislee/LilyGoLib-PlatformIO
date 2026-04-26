/**
 * @file      hw_sx1262.cpp
 * @brief     SX1262 (sub-GHz LoRa) — per-chip programming.
 *
 * The shared ISR/event-group/TX/RX/listening logic lives in
 * hal/radio_common.cpp; this file only owns the chip-specific pieces:
 * RadioLib `radio.set*()` calls, mode entry, default params, and the
 * frequency/bandwidth/power option tables exposed via `radio_get_*_list`.
 */

#include "../../hal_interface.h"
#include "../radio_chip.h"

#ifdef ARDUINO_LILYGO_LORA_SX1262

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

namespace radio_chip {

void default_params(radio_params_t &params)
{
    params.bandwidth = 125.0;
    params.freq      = RADIO_DEFAULT_FREQUENCY;
    params.cr        = 5;
    params.isRunning = false;
    params.mode      = RADIO_DISABLE;
    params.sf        = 12;
    params.power     = 22;
    params.interval  = 3000;
    params.syncWord  = 0xCD;
}

int16_t configure(const radio_params_t &params)
{
#ifdef ARDUINO
    int16_t state = radio.setFrequency(params.freq);
    if (state == RADIOLIB_ERR_INVALID_FREQUENCY) {
        Serial.println(F("Selected frequency is invalid for this module!"));
    }
    state = radio.setBandwidth(params.bandwidth);
    if (state == RADIOLIB_ERR_INVALID_BANDWIDTH) {
        Serial.println(F("Selected bandwidth is invalid for this module!"));
    }
    state = radio.setSpreadingFactor(params.sf);
    if (state == RADIOLIB_ERR_INVALID_SPREADING_FACTOR) {
        Serial.println(F("Selected spreading factor is invalid for this module!"));
    }
    state = radio.setCodingRate(params.cr);
    if (state == RADIOLIB_ERR_INVALID_CODING_RATE) {
        Serial.println(F("Selected coding rate is invalid for this module!"));
    }
    state = radio.setSyncWord(params.syncWord);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.println(F("Unable to set sync word!"));
    }
    state = radio.setOutputPower(params.power);
    if (state == RADIOLIB_ERR_INVALID_OUTPUT_POWER) {
        Serial.println(F("Selected output power is invalid for this module!"));
    }
    state = radio.setCurrentLimit(140);
    if (state == RADIOLIB_ERR_INVALID_CURRENT_LIMIT) {
        Serial.println(F("Selected current limit is invalid for this module!"));
    }

    switch (params.mode) {
    case RADIO_DISABLE: state = radio.standby();          break;
    case RADIO_TX:      state = radio.startTransmit(""); break;
    case RADIO_RX:      state = radio.startReceive();    break;
    case RADIO_CW:                                        break;
    default:                                              break;
    }
    return state;
#else
    (void)params;
    return 0;
#endif
}

} // namespace radio_chip


// ----- Option tables -----

static const float bandwidth_list[]   = {41.7, 62.5, 125.0, 250.0, 500.0};
static const float power_level_list[] = {2, 5, 10, 12, 17, 20, 22};
#ifdef RADIO_FIXED_FREQUENCY
static const float freq_list[] = {RADIO_FIXED_FREQUENCY};
#else
static const float freq_list[] = {433.0, 470.0, 842.0, 850, 868.0, 915.0, 923.0, 945.0};
#endif

uint16_t radio_get_freq_length()      { return sizeof(freq_list)        / sizeof(freq_list[0]); }
uint16_t radio_get_bandwidth_length() { return sizeof(bandwidth_list)   / sizeof(bandwidth_list[0]); }
uint16_t radio_get_tx_power_length()  { return sizeof(power_level_list) / sizeof(power_level_list[0]); }

float radio_get_freq_from_index(uint8_t index)
{
    if (index >= radio_get_freq_length()) return RADIO_DEFAULT_FREQUENCY;
    return freq_list[index];
}

float radio_get_bandwidth_from_index(uint8_t index)
{
    if (index >= radio_get_bandwidth_length()) return 125.0;
    return bandwidth_list[index];
}

float radio_get_tx_power_from_index(uint8_t index)
{
    if (index >= radio_get_tx_power_length()) return 22;
    return power_level_list[index];
}

#endif  // ARDUINO_LILYGO_LORA_SX1262
