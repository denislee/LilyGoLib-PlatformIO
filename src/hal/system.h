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

// Non-blocking NTP sync. Starts SNTP (requires WiFi up) and returns
// immediately; poll hw_get_time_sync_status() to detect completion. The
// RTC write happens in factory.ino's SNTP notification callback, so no
// follow-up work is required from the caller beyond refreshing any UI that
// reflects the current time.
//
// `gmt_offset_sec` overrides the compile-time GMT_OFFSET_SECOND constant —
// pass INT_MIN (or call the no-arg overload) to use the default. Settings
// flows that let the user pick a timezone should pass the resolved offset
// here so the synced wall-clock time matches the user's chosen city.
bool hw_start_time_sync_ntp();
bool hw_start_time_sync_ntp(int gmt_offset_sec);
// 1 = synced since last start, 0 = not yet (in progress or reset).
int  hw_get_time_sync_status();
// Called from the SNTP notification callback (factory.ino) to mark the
// sync as completed. Decouples UI feedback from sntp_get_sync_status(),
// which in SNTP_SYNC_MODE_IMMED often stays at RESET even after a
// successful update.
void hw_notify_time_sync_completed();

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
