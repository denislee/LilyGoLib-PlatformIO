/**
 * @file      system.h
 * @brief     System-level HAL: init, device info, power modes, settings, feedback.
 */
#pragma once

#include "types.h"

void hw_init();

uint16_t hw_get_devices_nums();
const char *hw_get_devices_name(int index);
const char *hw_get_variant_name();
bool hw_get_mac(uint8_t *mac);
uint32_t hw_get_device_online();
const char *hw_get_device_power_tips_string();
const char *hw_get_firmware_hash_string();
const char *hw_get_chip_id_string();
void hw_get_arduino_version(std::string &param);

void hw_get_date_time(std::string &param);
void hw_get_date_time(struct tm &timeinfo);
void hw_set_date_time(struct tm &timeinfo);

void hw_shutdown();
void hw_sleep();
void hw_light_sleep();
void hw_power_down_all();
void hw_power_up_all();
void hw_low_power_loop();
void hw_set_cpu_freq(uint32_t mhz);

void hw_feedback();
bool hw_get_haptic_enable();
void hw_set_haptic_enable(bool en);

void hw_get_user_setting(user_setting_params_t &param);
void hw_load_setting();
void hw_set_user_setting(user_setting_params_t &param);

void hw_print_mem_info();
void hw_get_heap_info(uint32_t &total, uint32_t &free);

void hw_disable_input_devices();
void hw_enable_input_devices();
void hw_set_trackball_callback(void(*callback)(uint8_t dir));
void hw_set_button_callback(void (*callback)(uint8_t idx, uint8_t state));

// UI helpers exposed from factory/ui_main for HAL-layer callers.
void ui_show_wifi_process_bar();
void ui_msg_pop_up(const char *title_txt, const char *msg_txt);
bool isinMenu();
