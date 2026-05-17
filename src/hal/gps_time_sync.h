/**
 * @file      gps_time_sync.h
 * @brief     GPS-based wall-clock sync. Counterpart to hw_start_time_sync_ntp.
 *
 * The GPS module is normally just a power toggle (see sensors.cpp). This
 * subsystem turns it on (transiently, if needed), parses incoming NMEA on
 * Serial1, and on the first valid date+time fix writes the result to the
 * RTC and system clock using the user's POSIX timezone rule for wall-clock
 * conversion.
 *
 * Usage: call hw_start_time_sync_gps(), then drive hw_pump_time_sync_gps()
 * from a UI timer at ~5Hz, and poll hw_get_time_sync_gps_status() for
 * completion. Call hw_stop_time_sync_gps() to cancel.
 */
#pragma once

// Returns true if the sync started. Powers on the GPS rail if it was off
// (and remembers so hw_stop_time_sync_gps can restore the prior state).
// Resets the parser; any partial NMEA from a previous attempt is discarded.
bool hw_start_time_sync_gps();

// Pumps NMEA bytes from Serial1 through the parser. On the first valid
// date+time it writes the clock and flips status to 1. After the deadline
// (~90s) flips status to -1.
void hw_pump_time_sync_gps();

// Status of the most recent attempt:
//   1  — synced
//   0  — in progress (or never started)
//  -1  — timed out without a valid fix
//  -2  — not supported (emulator build)
int  hw_get_time_sync_gps_status();

// Stop an in-flight sync. Idempotent. Restores GPS power if start() turned
// it on transiently.
void hw_stop_time_sync_gps();
