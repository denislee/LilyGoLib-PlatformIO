/**
 * @file      spi_lock.cpp
 * @brief     Implementation of core::ScopedSpiLock + MaybeSpiLock (see spi_lock.h).
 */
#include "spi_lock.h"

#ifdef ARDUINO
#include <LilyGoLib.h>

namespace core {

ScopedSpiLock::ScopedSpiLock()  { instance.lockSPI(); }
ScopedSpiLock::~ScopedSpiLock() { instance.unlockSPI(); }

MaybeSpiLock::MaybeSpiLock() : held_(false) {}
MaybeSpiLock::MaybeSpiLock(bool acquire_now) : held_(false) { if (acquire_now) acquire(); }
MaybeSpiLock::~MaybeSpiLock() { release(); }
void MaybeSpiLock::acquire() { if (!held_) { instance.lockSPI();   held_ = true;  } }
void MaybeSpiLock::release() { if (held_)  { instance.unlockSPI(); held_ = false; } }

} // namespace core

#else  // !ARDUINO — emulator has no SPI bus; all ops are no-ops.

namespace core {

ScopedSpiLock::ScopedSpiLock()  {}
ScopedSpiLock::~ScopedSpiLock() {}

MaybeSpiLock::MaybeSpiLock() : held_(false) {}
MaybeSpiLock::MaybeSpiLock(bool acquire_now) : held_(false) { if (acquire_now) held_ = true; }
MaybeSpiLock::~MaybeSpiLock() {}
void MaybeSpiLock::acquire() { held_ = true; }
void MaybeSpiLock::release() { held_ = false; }

} // namespace core

#endif  // ARDUINO
