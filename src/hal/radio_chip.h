/**
 * @file      radio_chip.h
 * @brief     Private per-chip interface used by radio_common.cpp.
 *
 * Each `hw_<chip>.cpp` selected at build time (sx1262 / sx1280 / lr1121 /
 * cc1101) implements the two functions below; `radio_common.cpp` holds the
 * shared ISR/event-group/TX/RX plumbing and calls into them.
 *
 * Not a public API — do not include from src/ui_*.cpp. This file is only
 * referenced by radio_common.cpp and the per-chip drivers.
 */
#pragma once

#include "types.h"

namespace radio_chip {

// Fill `p` with this chip's startup defaults (band, bandwidth, SF, CR, power,
// etc.). The common layer calls this from `hw_get_radio_params` so callers
// have sensible values without touching the hardware.
void default_params(radio_params_t &p);

// Apply `p` to the hardware and enter the requested mode. Caller holds the
// SPI lock. Returns the last RadioLib status code observed (RADIOLIB_ERR_NONE
// on success, the negative error code of the first failing call otherwise).
//
// Implementations must handle `p.mode == RADIO_DISABLE / RADIO_TX / RADIO_RX`
// and may special-case `RADIO_CW` where the chip supports it.
int16_t configure(const radio_params_t &p);

} // namespace radio_chip
