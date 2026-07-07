/**
 * @file      radio_common.cpp
 * @brief     Shared radio config + boot ISR wiring for every LoRa/FSK RadioLib driver.
 *
 * Per-chip programming (setFrequency, setBandwidth, mode entry, ...) lives in
 * src/hw_<chip>.cpp and is reached through the `radio_chip::` hooks in
 * radio_chip.h. Exactly one per-chip driver is selected at build time via
 * ARDUINO_LILYGO_LORA_<MODULE>; this file is compiled unconditionally.
 */

#include "radio.h"
#include "radio_chip.h"
#include "../core/spi_lock.h"

#ifdef ARDUINO
#include <LilyGoLib.h>

static EventGroupHandle_t radioEvent = NULL;

#define LORA_ISR_FLAG _BV(0)

static void hw_radio_isr()
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    BaseType_t xResult = xEventGroupSetBitsFromISR(
                             radioEvent, LORA_ISR_FLAG, &xHigherPriorityTaskWoken);
    if (xResult == pdPASS) {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void hw_radio_begin()
{
    radioEvent = xEventGroupCreate();
    radio.setPacketSentAction(hw_radio_isr);
}
#else
void hw_radio_begin() {}
#endif  // ARDUINO

int16_t hw_set_radio_params(radio_params_t &params)
{
    RADIO_LOG("Set radio params:\n");
    RADIO_LOG("Frequency:%.2f MHz\n", params.freq);
    RADIO_LOG("Bandwidth:%.2f KHz\n", params.bandwidth);
    RADIO_LOG("TxPower:%u dBm\n", params.power);
    RADIO_LOG("Interval:%u ms\n", params.interval);
    RADIO_LOG("CR:%u \n", params.cr);
    RADIO_LOG("SF:%u \n", params.sf);
    RADIO_LOG("SyncWord:%u \n", params.syncWord);
    RADIO_LOG("Mode: ");
    switch (params.mode) {
    case RADIO_DISABLE: RADIO_LOG("RADIO_DISABLE\n"); break;
    case RADIO_TX:      RADIO_LOG("RADIO_TX\n");      break;
    case RADIO_RX:      RADIO_LOG("RADIO_RX\n");      break;
    case RADIO_CW:      RADIO_LOG("RADIO_CW\n");      break;
    default:            break;
    }

#ifdef ARDUINO
    core::ScopedSpiLock lock;
    return radio_chip::configure(params);
#else
    (void)params;
    return 0;
#endif
}

void hw_get_radio_params(radio_params_t &params)
{
    radio_chip::default_params(params);
}

int16_t hw_set_radio_default()
{
    radio_params_t params;
    hw_get_radio_params(params);
    return hw_set_radio_params(params);
}
