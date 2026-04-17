/**
 * @file      peripherals.h
 * @brief     NFC and IR remote.
 */
#pragma once

#include "types.h"

// --- NFC ---
bool hw_get_nfc_enable();
void hw_set_nfc_enable(bool en);
bool hw_start_nfc_discovery();
void hw_stop_nfc_discovery();

// --- IR remote ---
#if defined(USING_IR_REMOTE)
void hw_set_remote_code(uint32_t nec_code);
#endif
void hw_ir_function_select(bool enableSend);
void hw_get_remote_code(uint64_t &result);
