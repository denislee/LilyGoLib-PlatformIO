/**
 * @file      spi_lock.h
 * @brief     RAII wrapper around the vendor's `instance.lockSPI()` /
 *            `instance.unlockSPI()` pair.
 *
 * Distinct from `core::ScopedInstanceLock` (scoped_lock.h):
 *  - `ScopedInstanceLock`  → our own top-level mutex, coordinating the LVGL
 *                            task, NFC task, keyboard task, and main loop.
 *  - `ScopedSpiLock`       → the vendor-internal SPI-bus lock. The vendor
 *                            uses it to serialize display / radio / SD
 *                            transactions on the shared SPI. On T-Watch-S3
 *                            it's a no-op; on T-LoRa-Pager and T-Watch-Ultra
 *                            it's a real mutex.
 *
 * Every site under `src/hal/storage.cpp` and the SD-touching paths of
 * `ui_audio_notes.cpp` used to call `instance.lockSPI()` / `unlockSPI()`
 * manually, which leaks the lock on any early return. Prefer this wrapper
 * for new code.
 *
 * Declared here without including LilyGoLib so the header is cheap to
 * include from HAL and UI TUs; the implementation lives in spi_lock.cpp.
 */
#pragma once

namespace core {

class ScopedSpiLock {
public:
    ScopedSpiLock();
    ~ScopedSpiLock();
    ScopedSpiLock(const ScopedSpiLock &) = delete;
    ScopedSpiLock &operator=(const ScopedSpiLock &) = delete;
    ScopedSpiLock(ScopedSpiLock &&) = delete;
    ScopedSpiLock &operator=(ScopedSpiLock &&) = delete;
};

} // namespace core
