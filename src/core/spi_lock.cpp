/**
 * @file      spi_lock.cpp
 * @brief     Implementation of core::ScopedSpiLock (see spi_lock.h).
 */
#include "spi_lock.h"

#ifdef ARDUINO
#include <LilyGoLib.h>

namespace core {
ScopedSpiLock::ScopedSpiLock()  { instance.lockSPI(); }
ScopedSpiLock::~ScopedSpiLock() { instance.unlockSPI(); }
} // namespace core

#else  // !ARDUINO — emulator has no SPI bus; ctor/dtor are no-ops.

namespace core {
ScopedSpiLock::ScopedSpiLock()  {}
ScopedSpiLock::~ScopedSpiLock() {}
} // namespace core

#endif  // ARDUINO
