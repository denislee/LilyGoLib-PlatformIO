/**
 * @file      radio.h
 * @brief     LoRa/FSK radio (sx126x/cc1101/sx128x/lr1121), NRF24, Si4735 FM, RF switch.
 */
#pragma once

#include "types.h"

// --- Primary radio module (LoRa / FSK) ---
bool hw_get_radio_enable();
void hw_set_radio_enable(bool en);

int16_t hw_set_radio_params(radio_params_t &params);
void hw_get_radio_params(radio_params_t &params);
void hw_set_radio_listening();
void hw_set_radio_default();
void hw_set_radio_tx(radio_tx_params_t &params, bool continuous = true);
void hw_get_radio_rx(radio_rx_params_t &params);

const char *radio_get_freq_list();
float radio_get_freq_from_index(uint8_t index);
uint16_t radio_get_freq_length();

const char *radio_get_bandwidth_list(bool high_freq = false);
float radio_get_bandwidth_from_index(uint8_t index);
uint16_t radio_get_bandwidth_length();

const char *radio_get_tx_power_list(bool high_freq = false);
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

// --- Si4735 FM/AM/SSB ---
void hw_si4735_set_power(bool powerOn);
void hw_si4735_set_volume(uint8_t vol);
uint8_t hw_si4735_get_volume(void);
uint8_t hw_si4735_get_rssi();
uint16_t hw_si4735_get_freq();
bool hw_si4735_is_fm();
void hw_si4735_set_mode(Si4735Mode bandType);
Si4735Mode hw_si4735_get_mode();
const char *hw_si4735_get_band_name();
uint16_t si4735_update_steps();
void si4735_set_agc(bool on);
void si4735_set_bfo(bool on);
void si4735_set_freq_up();
void si4735_set_freq_down();
void si4735_band_up();
void si4735_band_down();
uint16_t si4735_get_current_step();
