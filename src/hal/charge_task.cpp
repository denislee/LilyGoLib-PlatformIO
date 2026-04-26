/**
 * @file      charge_task.cpp
 * @brief     Charge-indicator overlay while in fake light sleep.
 *
 * When the user long-presses the rotary to put the device into fake light
 * sleep (display off, peripherals down, LVGL task idle — see
 * `lib/LilyGoLib/.../rotaryTask` and `ui_pause_timers`), we still want a
 * cable-plug event to surface as user-visible feedback. This task polls
 * VBUS on its own core-0 thread; on the rising edge while fake-sleep is
 * active it briefly wakes the display, throws up a "Charging XX%" overlay
 * for a few seconds, then puts the device back to sleep.
 *
 * The vendor rotary task owns its own `display_off` static, which we don't
 * touch — the round-trip ends with the display in the same logical state
 * as before, so a subsequent long-press still wakes correctly.
 */
#include "charge_task.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <LilyGoLib.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../core/scoped_lock.h"
#include "../core/system_hooks.h"
#include "../ui_define.h"
#include "internal.h"
#include "power.h"

namespace {

constexpr UBaseType_t kTaskPriority = 3;
constexpr BaseType_t  kTaskCore     = 0;
constexpr uint32_t    kPollMs       = 500;
constexpr uint32_t    kShowMs       = 4000;

TaskHandle_t s_task = nullptr;

bool poll_vbus_locked()
{
#if defined(USING_PPM_MANAGE)
    return instance.ppm.isVbusIn();
#elif defined(USING_PMU_MANAGE)
    return instance.pmu.isVbusIn();
#else
    return false;
#endif
}

lv_obj_t *build_charge_overlay()
{
    monitor_params_t p;
    hw_get_monitor_params(p);

    lv_obj_t *overlay = ui_popup_create(NULL);

    lv_obj_t *icon = lv_label_create(overlay);
    lv_label_set_text(icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(icon, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_text_font(icon, lv_theme_get_font_large(overlay), 0);

    lv_obj_t *pct = lv_label_create(overlay);
    lv_label_set_text_fmt(pct, "%d%%", p.battery_percent);
    lv_obj_set_style_text_color(pct, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(pct, lv_theme_get_font_large(overlay), 0);

    lv_obj_t *sub = lv_label_create(overlay);
    lv_label_set_text(sub, "Charging");
    lv_obj_set_style_text_color(sub, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(sub, get_small_font(), 0);

    return overlay;
}

void charge_task_fn(void *)
{
    bool prev_vbus = false;
    bool primed    = false;  // ignore first sample; we want edges, not boot state

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));

        bool cur_vbus;
        {
            core::ScopedInstanceLock lock;
            cur_vbus = poll_vbus_locked();
        }

        bool rising = primed && cur_vbus && !prev_vbus;
        prev_vbus = cur_vbus;
        primed = true;

        if (!rising) continue;
        if (!ui_is_fake_sleep()) continue;

        uint8_t target_brightness = user_setting.brightness_level;
        if (target_brightness == 0) target_brightness = 100;

        lv_obj_t *overlay = nullptr;
        {
            core::ScopedInstanceLock lock;
            instance.wakeupDisplay();
            instance.setBrightness(target_brightness);
            ui_resume_timers();
            lv_display_trigger_activity(NULL);
            overlay = build_charge_overlay();
        }

        // Hold the overlay on screen while LVGL renders. The instance lock
        // is released so the LVGL task can pump and flush.
        vTaskDelay(pdMS_TO_TICKS(kShowMs));

        {
            core::ScopedInstanceLock lock;
            if (overlay) ui_popup_destroy(overlay);
            ui_pause_timers();
            instance.setBrightness(0);
            instance.sleepDisplay();
        }

        // Refresh prev_vbus from a fresh sample so a still-asserted VBUS
        // doesn't immediately re-fire after we've shown the popup once.
        {
            core::ScopedInstanceLock lock;
            prev_vbus = poll_vbus_locked();
        }
    }
}

}  // namespace

void hw_charge_task_start()
{
    if (s_task) return;
    BaseType_t ok = xTaskCreatePinnedToCore(
        charge_task_fn, "charge_ind", 4096, nullptr,
        kTaskPriority, &s_task, kTaskCore);
    if (ok != pdPASS) {
        log_e("charge_task: task create failed");
        s_task = nullptr;
    }
}

#else  // !ARDUINO

void hw_charge_task_start() {}

#endif  // ARDUINO
