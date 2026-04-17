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

// Back-compat: previous revisions of this header exposed `string`/`vector`
// unqualified. Keep the using-directives here so existing .cpp files that
// include hal_interface.h compile unchanged. New code should include the
// specific hal/ sub-header and use std::-qualified names.
using std::string;
using std::vector;
