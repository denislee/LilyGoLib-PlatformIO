<div align="center" markdown="1">
  <img src=".github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGoLib-PlatformIO🌟</h1>

# `1` Overview

* This repository demonstrates how [LilyGoLib](https://github.com/Xinyuan-LilyGO/LilyGoLib) uses [PlatformIO](https://platformio.org/)

# `2` Features

### 📱 Core Applications & Utilities
*   **Telegram Client**: Background polling and messaging support.
*   **Weather App**: Displays local weather forecasts.
*   **Media Remote**: Control media playback.
*   **Audio Notes / Voice Recorder**: Uses the onboard microphone to record notes and `libhelix-mp3` for MP3 playback.
*   **Text Editor & Journal**: Write and view text files and journal entries.
*   **Notes & Secure Sync**: Note management with encrypted credentials.
*   **Task Manager**: Keep track of to-do items.
*   **File Browser**: Explore files stored on the device's filesystem (SD Card/SPIFFS/LittleFS).
*   **System Settings**: Comprehensive on-device configuration (Display, Connectivity, Fonts, Storage, Time/Date, Weather, Telegram).

### 🛠️ System Architecture & UI
*   **LVGL v9 Integration**: High-quality, modern, and modular graphical user interface components.
*   **Multitasking & App Registry**: App Manager that handles app lifecycles, memory cleanup, and input focus to safely switch between rich UI apps.
*   **Custom Typography**: Built-in support for highly readable fonts including Atkinson Hyperlegible, JetBrains Mono, and Courier Prime.
*   **Thread-Safe Hardware Access**: Mutex-protected SPI bus access to prevent collisions between the UI rendering loop and radio/NFC background interrupts.
*   **Desktop Emulator Support**: Features an SDL2 emulator allowing developers to test and iterate on the UI on Linux, macOS, or Windows without needing physical hardware.

### 📻 Hardware & Radio Support
*   **Unified Hardware Abstraction Layer (HAL)**: Decouples the UI logic from the physical hardware, ensuring code portability.
*   **Multi-Device Targets**: Native support out-of-the-box for LilyGo T-LoRa-Pager, T-Watch-S3, and T-Watch-Ultra.
*   **Flexible Radio Modules**: Abstracted radio interface supporting multiple chips via configuration: SX1262, CC1101, SX1280, LR1121, SI4432.
*   **NFC Provisioning**: Tap-to-configure or data transfer features utilizing onboard NFC readers.
*   **Advanced Power Management**: Battery protection features (like limiting charging to 80% to preserve battery lifespan) and deep sleep capabilities.

# `3` Platformio IDE Quick Start

1. Install [Visual Studio Code](https://code.visualstudio.com/) and [Python](https://www.python.org/)
2. Search for the `PlatformIO` plugin in the `Visual Studio Code` extension and install it.
3. After the installation is complete, you need to restart `Visual Studio Code`
4. After restarting `Visual Studio Code`, select `File` in the upper left corner of `Visual Studio Code` -> `Open Folder` -> select the `LilyGoLib-PlatformIO` directory
5. Wait for the installation of third-party dependent libraries to complete
6. Click on the `platformio.ini` file, and in the `platformio` column
7. Select the board name you want to use in `default_envs` and uncomment it.
8. The default compiled sketch is [main.cpp](./src/main.cpp) in the src directory. If you need to compile an example in LilyGoLib, uncomment one of the lines src_dir = examples/xxxxx to enable it and make sure only one line is valid.
9. Click the (✔) symbol in the lower left corner to compile
10. Connect the board to the computer USB
11. Click (→) to upload firmware
12. Click (plug symbol) to monitor serial output

> \[!IMPORTANT]
>
> ⚠️ USB ports keep popping in and out?
>
> * T-Watch-S3 see [here](https://github.com/Xinyuan-LilyGO/LilyGoLib/blob/master/docs/lilygo-t-watch-s3.md#t-watch-s3-enter-download-mode)
> * T-Watch-S3-Plus see  [here](https://github.com/Xinyuan-LilyGO/LilyGoLib/blob/master/docs/lilygo-t-watch-s3-plus.md#t-watch-s3-plus-enter-download-mode)
> * T-Watch-Ultra see [here](https://github.com/Xinyuan-LilyGO/LilyGoLib/blob/master/docs/lilygo-t-watch-ultra.md#t-watch-s3-ultra-enter-download-mode)
> * T-LoRa-Pager see [here](https://github.com/Xinyuan-LilyGO/LilyGoLib/blob/master/docs/lilygo-t-lora-pager.md#t-lora-pager-enter-download-mode)
>
> 💠 Quick troubleshooting
> Write the factory [firmware](https://github.com/Xinyuan-LilyGO/LilyGoLib/tree/master/firmware) we provide for hardware diagnosis
