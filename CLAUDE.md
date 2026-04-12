# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PlatformIO-based firmware for LilyGo ESP32-S3 devices (T-LoRa-Pager, T-Watch-S3, T-Watch-Ultra). Uses the Arduino framework with LVGL v9 for GUI rendering. The same codebase also compiles as an SDL2 desktop emulator for UI development without hardware.

## Build Commands

```bash
# Build (default env: tlora_pager)
pio run

# Build specific environment
pio run -e tlora_pager
pio run -e twatchs3
pio run -e twatch_ultra

# Upload to device
pio run -e tlora_pager --target upload

# Serial monitor (115200 baud)
pio device monitor

# Build SDL2 emulator (requires SDL2 dev libs)
pio run -e emulator_lora_pager
pio run -e emulator_twatchs3
pio run -e emulator_watch_ultra
```

There are no test targets or linting configured in this project.

## Selecting Radio Module

Only one radio module can be active at build time. In `platformio.ini` under `[env_arduino]` build_flags, uncomment exactly one:

```
-D ARDUINO_LILYGO_LORA_SX1262   (default)
-D ARDUINO_LILYGO_LORA_CC1101
-D ARDUINO_LILYGO_LORA_SX1280
-D ARDUINO_LILYGO_LORA_LR1121
-D ARDUINO_LILYGO_LORA_SI4432
```

## Selecting Source Directory

By default `src/` is compiled. To compile a LilyGoLib example instead, uncomment one `src_dir` line in `platformio.ini` under `[platformio]` and comment out `src_dir = src`.

## Architecture

### Entry Points
- **`src/factory.ino`** - Arduino entry point (`setup()` / `loop()`). Initializes LilyGoLib hardware instance, LVGL, HAL, and GUI. The main loop runs LVGL timer handler and NFC polling inside a FreeRTOS mutex.
- **`src/main.cpp`** - SDL2 emulator entry point. Provides a desktop simulation of the UI without hardware.

### Hardware Abstraction Layer (HAL)
- **`src/hal_interface.h`** - Declares 100+ functions that abstract all hardware: radio, GPS, power, audio, WiFi, display, sensors, NFC, keyboard, storage. Defines hardware status bitmask constants (`HW_RADIO_ONLINE`, `HW_NFC_ONLINE`, etc.) and parameter structs.
- **`src/hal_interface.cpp`** - Implements HAL functions using LilyGoLib APIs. Handles NVS settings persistence, battery monitoring, audio playback (MP3 decode via FreeRTOS task), WiFi management, file system abstraction (SD card / FFat).

### Radio Module Drivers
Each file implements the same set of functions (`hw_radio_begin`, `hw_set_radio_params`, `hw_set_radio_listening`, `hw_set_radio_tx`, `hw_get_radio_rx`, etc.) guarded by `#ifdef ARDUINO_LILYGO_LORA_<MODULE>`:
- `src/hw_sx1262.cpp` - SX1262 LoRa (sub-GHz)
- `src/hw_cc1101.cpp` - CC1101 FSK/OOK
- `src/hw_sx1280.cpp` - SX1280 2.4GHz LoRa
- `src/hw_lr1121.cpp` - LR1121 multi-band LoRa
- `src/hw_nrf2401.cpp` - nRF24L01 2.4GHz

All radio drivers use interrupt-driven TX/RX with FreeRTOS EventGroups (`LORA_ISR_FLAG`).

### NFC
- `src/app_nfc.h` / `src/app_nfc.cpp` - NFC reader/writer (URL, WiFi config, text, device info). Only available on T-LoRa-Pager and T-Watch-Ultra.

### UI Layer
Each feature has a dedicated `src/ui_*.cpp` file with `setup_func_cb` and `exit_func_cb` callbacks for clean lifecycle management:
- `ui_main.cpp` - Main menu system
- `ui_radio.cpp` - Radio TX/RX interface (most complex UI module)
- `ui_nrf24.cpp`, `ui_msgchat.cpp`, `ui_wireless.cpp`, `ui_ble.cpp`, `ui_gps.cpp`, `ui_audio.cpp`, `ui_sensor.cpp`, `ui_nfc.cpp`, `ui_monitor.cpp`, `ui_sys.cpp`, `ui_theme.cpp`, etc.
- `ui_define.h` - Shared UI types and macros

UI code calls only HAL functions, never LilyGoLib directly.

### Assets
- `src/images/` - Source image files
- `src/src/` - LVGL-compatible C arrays for images and fonts (auto-generated, large files)

### Board Definitions
- `boards/` - Custom PlatformIO board JSON files
- `variants/` - Board-specific `pins_arduino.h` pin definitions

## Key Patterns

- **Thread safety**: All LilyGoLib SPI bus access is protected by `instanceLockTake()` / `instanceLockGive()` mutex calls in the main loop and radio ISRs.
- **Conditional compilation**: Board-specific code uses `ARDUINO_T_LORA_PAGER`, `ARDUINO_T_WATCH_S3`, `ARDUINO_T_WATCH_S3_ULTRA` defines. Radio module code uses `ARDUINO_LILYGO_LORA_*` defines.
- **Settings persistence**: User preferences stored via Arduino `Preferences` library (NVS flash).
- **Partition table**: Uses the partition CSV from LilyGoLib's factory example, not the default.

## Key Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| LilyGoLib | latest | Main hardware abstraction (from GitHub) |
| LVGL | 9.4.0 | GUI framework |
| RadioLib | 7.4.0 | Radio module driver abstraction |
| XPowersLib | 0.3.1 | Power management IC driver |
| SensorLib | 0.3.3 | IMU/sensor drivers |
| TinyGPSPlus | 1.0.3 | GPS NMEA parsing |
| NimBLE-Arduino | 2.2.3 | BLE stack |
| ESP8266Audio | 2.0.0 | Audio playback |
| ST25R3916-fork | latest | NFC controller (tlora_pager, twatch_ultra only) |
