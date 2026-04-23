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

/* True iff the last hw_start_nfc_discovery() call succeeded and we haven't
 * since stopped. Distinguishes "chip powered" from "RFAL polling live". */
bool hw_nfc_discovery_active();

/* Diagnostic hook invoked on every NFC event, in the LVGL thread.
 * `kind` values:
 *   0 = raw detection (before NDEF parse)
 *   1 = RTD Text
 *   2 = RTD URI
 *   3 = Media WiFi
 *   4 = Other / unhandled NDEF type
 * `text` / `len` carry a short preview payload or NULL/0. Pass NULL to clear. */
typedef void (*nfc_diag_cb_t)(int kind, const char *text, unsigned len);
void hw_set_nfc_diag_hook(nfc_diag_cb_t cb);

// --- IR remote ---
#if defined(USING_IR_REMOTE)
void hw_set_remote_code(uint32_t nec_code);
#endif
void hw_ir_function_select(bool enableSend);
void hw_get_remote_code(uint64_t &result);
