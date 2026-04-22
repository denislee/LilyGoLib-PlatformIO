/**
 * @file      radio_common.cpp
 * @brief     Shared TX/RX/ISR plumbing for every LoRa/FSK RadioLib driver.
 *
 * Per-chip programming (setFrequency, setBandwidth, mode entry, ...) lives in
 * src/hw_<chip>.cpp and is reached through the `radio_chip::` hooks in
 * radio_chip.h. Exactly one per-chip driver is selected at build time via
 * ARDUINO_LILYGO_LORA_<MODULE>; this file is compiled unconditionally.
 */

#include "radio.h"
#include "radio_chip.h"

#ifdef ARDUINO
#include <LilyGoLib.h>

static EventGroupHandle_t radioEvent = NULL;
static uint32_t last_send_millis = 0;

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
    instance.lockSPI();
    int16_t state = radio_chip::configure(params);
    instance.unlockSPI();
    return state;
#else
    (void)params;
    return 0;
#endif
}

void hw_get_radio_params(radio_params_t &params)
{
    radio_chip::default_params(params);
}

void hw_set_radio_default()
{
    radio_params_t params;
    hw_get_radio_params(params);
    hw_set_radio_params(params);
}

void hw_set_radio_listening()
{
#ifdef ARDUINO
    instance.lockSPI();
    radio.startReceive();
    instance.unlockSPI();
#endif
}

void hw_set_radio_tx(radio_tx_params_t &params, bool continuous)
{
#ifdef ARDUINO
    if (continuous) {
        EventBits_t eventBits = xEventGroupWaitBits(
                                    radioEvent, LORA_ISR_FLAG,
                                    pdTRUE, pdTRUE, pdTICKS_TO_MS(2));
        if ((eventBits & LORA_ISR_FLAG) != LORA_ISR_FLAG) {
            params.state = -1;
            return;
        }
    }

    if (!params.data) {
        params.state = -1;
        return;
    }

    // Both calls touch the SPI bus, so they must sit inside the instance lock.
    // Pre-refactor, the SX1262 path ran finishTransmit() outside the lock —
    // a latent race against other SPI users (e.g. the display flush task).
    instance.lockSPI();
    radio.finishTransmit();
    params.state = radio.startTransmit(params.data, params.length);
    instance.unlockSPI();
#else
    (void)params;
    (void)continuous;
#endif
}

void hw_get_radio_rx(radio_rx_params_t &params)
{
#ifdef ARDUINO
    EventBits_t eventBits = xEventGroupWaitBits(
                                radioEvent, LORA_ISR_FLAG,
                                pdTRUE, pdTRUE, pdTICKS_TO_MS(2));
    if ((eventBits & LORA_ISR_FLAG) != LORA_ISR_FLAG) {
        params.state = -1;
        return;
    }

    if (!params.data) {
        params.state = -1;
        printf("Rx data buffer is empty\n");
        return;
    }

    instance.lockSPI();
    params.length = radio.getPacketLength();
    params.state  = radio.readData(params.data, params.length);
    params.rssi   = radio.getRSSI();
    params.snr    = radio.getSNR();
    // Re-arm receive before releasing the lock so we don't drop the next packet.
    radio.startReceive();
    instance.unlockSPI();

    // Suppress echoes of our own just-transmitted packet.
    if (last_send_millis + 200 > millis()) {
        params.length = 0;
        return;
    }

    params.data[params.length] = '\0';
#else
    params.length = 0;
#endif
}

bool radio_transmit(const uint8_t *data, size_t length)
{
#ifdef ARDUINO
    int state = radio.transmit(data, length);
    last_send_millis = millis();
    return (state == RADIOLIB_ERR_NONE);
#else
    (void)data;
    (void)length;
    return true;
#endif
}
