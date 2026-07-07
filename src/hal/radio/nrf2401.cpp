/**
 * @file      hw_nrf2401.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-04-24
 *
 */

#include "../../hal_interface.h"
#include "../../core/spi_lock.h"

#if defined(USING_EXTERN_NRF2401)

#ifdef ARDUINO

#include <LilyGoLib.h>

static EventGroupHandle_t    radioEvent = NULL;

#define NRF24_ISR_FLAG              _BV(1)

static void hw_nrf24_isr()
{
    BaseType_t xHigherPriorityTaskWoken, xResult;
    xHigherPriorityTaskWoken = pdFALSE;
    xResult = xEventGroupSetBitsFromISR(
                  radioEvent,
                  NRF24_ISR_FLAG,
                  &xHigherPriorityTaskWoken);
    if ( xResult == pdPASS ) {
        portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
    }
}

void hw_nrf24_begin()
{
    radioEvent = xEventGroupCreate();
    printf(" init NRF2401 \n");
    bool rlst = instance.initNRF24();
    if (!rlst) {
        printf("nRF2401 option Model not detected\n");
        return;
    }
    nrf24.setPacketSentAction(hw_nrf24_isr);

    // Set PA control IO to output function
    instance.io.pinMode(EXPANDS_GPIO_EN, OUTPUT);
}
#endif

#endif
