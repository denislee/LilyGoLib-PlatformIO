/**
 * @file      main.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-08
 *
 */

#ifndef ARDUINO
#include <stdio.h>
#include "lvgl.h"
#include <unistd.h>
#define SDL_MAIN_HANDLED        /*To fix SDL's "undefined reference to WinMain" issue*/
#include <SDL2/SDL.h>
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_mousewheel.h"
#include "drivers/sdl/lv_sdl_keyboard.h"
#include "demos/lv_demos.h"
#include "examples/lv_examples.h"

#include "hal_interface.h"
#include "core/system.h"
#include "apps/app_registry.h"

extern void hw_init();

// On hardware, factory.ino provides these (they guard the shared SPI bus).
// The emulator has no SPI bus and no background tasks contending for it, so
// they're intentional no-ops here.
void instanceLockTake() {}
void instanceLockGive() {}

// Power-state descriptor is meaningless on the host; return a stable empty
// string so the on-device power screen still renders without reaching into
// hardware registers.
const char *hw_get_device_power_tips_string() { return ""; }

static lv_display_t *lvDisplay;
static lv_indev_t *lvMouse;
static lv_indev_t *lvMouseWheel;
static lv_indev_t *lvKeyboard;


#if LV_USE_LOG != 0
static void lv_log_print_g_cb(lv_log_level_t level, const char * buf)
{
    LV_UNUSED(level);
    LV_UNUSED(buf);
}
#endif


void hal_setup(void)
{
    // Workaround for sdl2 `-m32` crash
    // https://bugs.launchpad.net/ubuntu/+source/libsdl2/+bug/1775067/comments/7
#ifndef WIN32
    setenv("DBUS_FATAL_WARNINGS", "0", 1);
#endif

#if LV_USE_LOG != 0
    lv_log_register_print_cb(lv_log_print_g_cb);
#endif

    /* Add a display
     * Use the 'monitor' driver which creates window on PC's monitor to simulate a display*/


    lvDisplay = lv_sdl_window_create(SDL_HOR_RES, SDL_VER_RES);
    lvMouse = lv_sdl_mouse_create();
    lvMouseWheel = lv_sdl_mousewheel_create();
    lvKeyboard = lv_sdl_keyboard_create();
}

void hal_loop(void)
{
    Uint32 lastTick = SDL_GetTicks();
    while (1) {
        fflush(stdout);
        SDL_Delay(5);
        Uint32 current = SDL_GetTicks();
        lv_tick_inc(current - lastTick);
        lastTick = current;
        lv_timer_handler();
        core::System::getInstance().loop();
    }
}


extern "C" int main(void)
{
    lv_init();

    hal_setup();
    printf("hello lvgl\n");
    hw_init();

    apps::register_all();
    core::System::getInstance().init();

    hal_loop();
}
#endif
