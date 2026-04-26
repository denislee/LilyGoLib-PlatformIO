/**
 * @file      radio.h
 * @brief     LoRa/FSK radio (sx126x/cc1101/sx128x/lr1121), NRF24, RF switch.
 */
#pragma once

#include "types.h"

// --- Primary radio module (LoRa / FSK) ---
bool hw_get_radio_enable();
// Returns RADIOLIB_ERR_NONE (0) on success, or a negative RadioLib status
// code from the underlying hw_set_radio_params() call on failure. The
// persisted user_setting is updated regardless so the user's intent is
// remembered across boots even if the hardware rejected it this time.
int16_t hw_set_radio_enable(bool en);

int16_t hw_set_radio_params(radio_params_t &params);
void hw_get_radio_params(radio_params_t &params);
void hw_set_radio_listening();
int16_t hw_set_radio_default();
void hw_set_radio_tx(radio_tx_params_t &params, bool continuous = true);
void hw_get_radio_rx(radio_rx_params_t &params);

float radio_get_freq_from_index(uint8_t index);
uint16_t radio_get_freq_length();

float radio_get_bandwidth_from_index(uint8_t index);
uint16_t radio_get_bandwidth_length();

float radio_get_tx_power_from_index(uint8_t index);
uint16_t radio_get_tx_power_length();

bool radio_transmit(const uint8_t *data, size_t length);

void hw_set_usb_rf_switch(bool to_usb);

// --- NRF24 (2.4 GHz) ---
void hw_get_nrf24_params(radio_params_t &params);
int16_t hw_set_nrf24_params(radio_params_t &params);
void hw_set_nrf24_listening();
bool hw_set_nrf24_tx(radio_tx_params_t &params, bool continuous = true);
void hw_get_nrf24_rx(radio_rx_params_t &params);
bool hw_has_nrf24();
void hw_clear_nrf24_flag();

