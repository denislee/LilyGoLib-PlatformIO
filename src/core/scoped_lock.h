/**
 * @file      scoped_lock.h
 * @brief     RAII wrapper around instanceLockTake / instanceLockGive.
 *
 * The instance mutex guards the LilyGoLib shared SPI bus (radio, display,
 * storage). Every non-trivial return path must release it, so prefer the
 * RAII `ScopedInstanceLock` over manual take/give pairs.
 */
#pragma once

void instanceLockTake();
void instanceLockGive();

namespace core {

class ScopedInstanceLock {
public:
    ScopedInstanceLock() { instanceLockTake(); }
    ~ScopedInstanceLock() { instanceLockGive(); }
    ScopedInstanceLock(const ScopedInstanceLock &) = delete;
    ScopedInstanceLock &operator=(const ScopedInstanceLock &) = delete;
    ScopedInstanceLock(ScopedInstanceLock &&) = delete;
    ScopedInstanceLock &operator=(ScopedInstanceLock &&) = delete;
};

} // namespace core
