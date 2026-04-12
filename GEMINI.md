# LilyGoLib-PlatformIO Project Overview

This project is a PlatformIO-based firmware for LilyGo ESP32-S3 devices, specifically targeting the **T-LoRa-Pager**, **T-Watch-S3**, and **T-Watch-Ultra**. It leverages the `LilyGoLib` hardware abstraction library and uses **LVGL v9** for high-quality GUI rendering.

The codebase is highly portable, supporting multiple hardware variants and radio modules through conditional compilation, and even includes an **SDL2 desktop emulator** for rapid UI development without physical hardware.

## Key Hardware Targets
- **T-LoRa-Pager**: A LoRa-enabled messaging device with a keyboard and small display.
- **T-Watch S3**: An ESP32-S3 based smartwatch with a square touch display.
- **T-Watch Ultra**: A premium smartwatch with a large round display and multi-band radio support.

## Supported Radio Modules
The project abstracts multiple radio chips through a common HAL. Only one can be active at a time (configured in `platformio.ini`):
- **SX1262**: LoRa (Sub-GHz) - Default
- **CC1101**: FSK/OOK
- **SX1280**: 2.4GHz LoRa
- **LR1121**: Multi-band LoRa
- **SI4432**: SI4432 Radio

## Project Structure

### Core Directories
- `src/`: Main application source code.
  - `src/ui_*.cpp`: Modular UI components (Main Menu, Radio, GPS, NFC, etc.).
  - `src/hal_interface.*`: Unified hardware abstraction layer.
  - `src/hw_*.cpp`: Specific radio module implementations.
- `lib/`: Local libraries, including a copy of `LilyGoLib` and `libhelix-mp3`.
- `boards/`: Custom PlatformIO board definition files.
- `variants/`: Board-specific pin definitions (`pins_arduino.h`).
- `support/`: Scripts for the SDL2 emulator build process.

### Architecture Highlights
- **HAL (Hardware Abstraction Layer)**: All UI code interacts with hardware through `hal_interface.h`, ensuring decoupling from specific hardware implementations.
- **Thread Safety**: SPI bus access is protected by mutexes (`instanceLockTake`/`instanceLockGive`) to prevent conflicts between the main UI loop and radio/NFC interrupts.
- **Lifecycle Management**: UI modules use `setup_func_cb` and `exit_func_cb` for clean memory and resource management when switching between apps.
- **Persistence**: User settings (brightness, charger limits, etc.) are saved to NVS flash using the Arduino `Preferences` library.

## Building and Running

### Build Commands
This project uses **PlatformIO CLI**. Ensure you have the `platformio` tool installed.

```bash
# Build the default environment (tlora_pager)
pio run

# Build for a specific device
pio run -e tlora_pager
pio run -e twatchs3
pio run -e twatch_ultra

# Upload firmware to a connected device
pio run -e tlora_pager --target upload

# Monitor serial output (115200 baud)
pio device monitor
```

### SDL2 Emulator
For UI development on a PC (Linux/macOS/Windows):
```bash
# Requires SDL2 development libraries installed on your system
pio run -e emulator_lora_pager
pio run -e emulator_twatchs3
pio run -e emulator_watch_ultra
```

## Development Conventions

### Coding Style
- **Conditional Compilation**: Use `ARDUINO_T_LORA_PAGER`, `ARDUINO_T_WATCH_S3`, or `ARDUINO_T_WATCH_S3_ULTRA` for board-specific logic.
- **UI Decoupling**: Never call `instance` or `LilyGoLib` directly from `ui_*.cpp` files. Always use the `hw_*` functions defined in `hal_interface.h`.
- **Memory Management**: When creating UI elements, ensure they are properly cleaned up in the `exit_func_cb` or through LVGL's parent-child deletion mechanism.
- **Battery Conservation**: A feature exists to limit charging to 80% to extend battery life (configurable in System Settings).

### Radio Selection
To change the active radio module, modify `build_flags` in `platformio.ini`:
```ini
build_flags =
    -D ARDUINO_LILYGO_LORA_SX1262   ; Uncomment exactly one
    ; -D ARDUINO_LILYGO_LORA_CC1101
```

### Compilation of Examples
To compile a specific example from `LilyGoLib` instead of the main application:
1. Open `platformio.ini`.
2. Uncomment the desired `src_dir` line under the `[platformio]` section.
3. Comment out `src_dir = src`.
