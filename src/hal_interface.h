/**
 * @file      hal_interface.h
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-08
 *
 * @brief     Umbrella header for the Hardware Abstraction Layer.
 *
 * The HAL is now split into per-domain headers under `hal/`. Prefer including
 * the specific header you need (`hal/audio.h`, `hal/radio.h`, ...) in new code;
 * this umbrella exists for backward compatibility with older translation units.
 */
#pragma once

#include "hal/types.h"
#include "hal/system.h"
#include "hal/display.h"
#include "hal/power.h"
#include "hal/storage.h"
#include "hal/audio.h"
#include "hal/wireless.h"
#include "hal/radio.h"
#include "hal/sensors.h"
#include "hal/peripherals.h"
#include "hal/board_config.h"

// This umbrella header used to expose `using std::string;` and
// `using std::vector;` for back-compat. Those leaked into every consumer
// and made namespace hygiene impossible. New code must use std::-qualified
// names; .cpp files that still rely on unqualified `string`/`vector` should
// add file-local `using std::string;` after their includes.
