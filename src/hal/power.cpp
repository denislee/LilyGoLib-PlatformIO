/**
 * @file      power.cpp
 * @brief     Battery, charger, OTG, monitor.
 */
#include "power.h"
#include "system.h"
#include "internal.h"

#include <lvgl.h>

#ifdef ARDUINO
#include <LilyGoLib.h>
#else
#include <cstdlib>
#include <cstring>
#endif

// --- Battery history ---------------------------------------------------

static std::vector<int16_t> battery_history;
static const size_t MAX_BATTERY_HISTORY = 60; // 5 hours if 5 mins interval

void hw_update_battery_history()
{
#ifdef ARDUINO
    monitor_params_t params;
    hw_get_monitor_params(params);
    int16_t percent = params.battery_percent;
    if (percent >= 0 && percent <= 100) {
        if (battery_history.size() >= MAX_BATTERY_HISTORY) {
            battery_history.erase(battery_history.begin());
        }
        battery_history.push_back(percent);
        log_d("Recorded battery percent: %d%%", percent);

        // Charge conservation logic: Stop charging if >= 80% and feature is enabled
        if (user_setting.charge_limit_en) {
            if (percent >= 80) {
                if (hw_get_charge_enable()) {
                    log_i("Battery life conservation: Reached %d%%, stopping charger.", percent);
                    hw_set_charger(false);
                }
            } else if (percent < 75) {
                // Re-enable charging if it drops below 75% while the limit feature is on
                if (user_setting.charger_enable && !hw_get_charge_enable()) {
                    log_i("Battery life conservation: Below 75%% (%d%%), re-enabling charger.", percent);
                    hw_set_charger(true);
                }
            }
        } else {
            // Limit disabled: Ensure charger follows the main charger_enable setting
            if (user_setting.charger_enable != hw_get_charge_enable()) {
                hw_set_charger(user_setting.charger_enable);
            }
        }
    }
#else
    static int16_t sim_percent = 100;
    if (battery_history.size() >= MAX_BATTERY_HISTORY) {
        battery_history.erase(battery_history.begin());
    }
    battery_history.push_back(sim_percent);
    sim_percent -= 2;
    if (sim_percent < 10) sim_percent = 100;
#endif
}

void hw_get_battery_history(std::vector<int16_t> &history)
{
    history = battery_history;
}

void battery_history_timer_cb(lv_timer_t *timer)
{
    hw_update_battery_history();
}

// --- Battery voltage ---------------------------------------------------

int16_t hw_get_battery_voltage()
{
#ifdef ARDUINO

#if  defined(USING_BQ_GAUGE)
    if (HW_GAUGE_ONLINE & hw_get_device_online()) {
        instance.gauge.refresh();
        return instance.gauge.getVoltage();
    } else {
        printf("Gauge Not online !\n");
        return 0;
    }
#elif defined(USING_PMU_MANAGE)
    return instance.pmu.getBattVoltage();
#else
    return 0;
#endif

#else
    return 0;
#endif
}

// --- OTG ---------------------------------------------------------------

bool hw_get_otg_enable()
{
#if defined(ARDUINO) && defined(USING_PPM_MANAGE)
    return  instance.ppm.isEnableOTG();
#else
    return false;
#endif
}

bool hw_set_otg(bool enable)
{
#if defined(ARDUINO) && defined(USING_PPM_MANAGE)
    if (enable) {
        return  instance.ppm.enableOTG();
    } else {
        instance.ppm.disableOTG();
    }
    return true;
#endif
    return false;
}

bool hw_has_otg_function()
{
#if defined(USING_PPM_MANAGE)
    return true;
#else
    return true;
#endif
}

// --- Charger -----------------------------------------------------------

bool hw_get_charge_enable()
{
#ifdef ARDUINO
#if defined(USING_PPM_MANAGE)
    return  instance.ppm.isEnableCharge();
#elif defined(USING_PMU_MANAGE)
    return  instance.isEnableCharge();
#endif
#else
    return false;
#endif
}

void hw_set_charger(bool enable)
{
#ifdef ARDUINO
#if defined(USING_PPM_MANAGE)
    if (enable) {
        instance.ppm.enableCharge();
    } else {
        instance.ppm.disableCharge();
    }
#elif defined(USING_PMU_MANAGE)
    if (enable) {
        instance.enableCharge();
    } else {
        instance.disableCharge();
    }
#endif
#endif
}

uint16_t hw_get_charger_current()
{
#ifdef ARDUINO
#if defined(USING_PPM_MANAGE)
    return  instance.ppm.getChargerConstantCurr();
#elif defined(USING_PMU_MANAGE)
    return  instance.getChargeCurrent();
#endif
#else
    return 0;
#endif
}

void hw_set_charger_current(uint16_t milliampere)
{
#ifdef ARDUINO
#if defined(USING_PPM_MANAGE)
    instance.ppm.setChargerConstantCurr(milliampere);
#elif defined(USING_PMU_MANAGE)
    instance.setChargeCurrent(milliampere);
#endif
#endif
}

uint8_t hw_get_charger_current_level()
{
#if defined(USING_PPM_MANAGE)
    return user_setting.charger_current / dev_conts_var.charge_steps;
#elif defined(USING_PMU_MANAGE)
    const uint16_t table[] = {
        100, 125, 150, 175,
        200, 300, 400, 500,
        600, 700, 800, 900,
        1000
    };
    uint16_t cur =  instance.getChargeCurrent();
    for (int i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (cur == table[i]) {
            return i;
        }
    }
    return 0;
#else
    const uint16_t table[] = {
        100, 125, 150, 175,
        200, 300, 400, 500,
        600, 700, 800, 900,
        1000
    };
    uint16_t cur =  user_setting.charger_current;
    for (int i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (cur == table[i]) {
            return i;
        }
    }
    return 0;
#endif
}

uint16_t hw_set_charger_current_level(uint8_t level)
{
#ifdef ARDUINO
#if defined(USING_PPM_MANAGE)
    printf("set charge current:%u mA\n", level * dev_conts_var.charge_steps);
    instance.ppm.setChargerConstantCurr(level * dev_conts_var.charge_steps);
    return  level * dev_conts_var.charge_steps;
#elif defined(USING_PMU_MANAGE)
    const uint16_t table[] = {
        100, 125, 150, 175,
        200, 300, 400, 500,
        600, 700, 800, 900,
        1000
    };
    if (level > (sizeof(table) / sizeof(table[0]) - 1)) {
        level = sizeof(table) / sizeof(table[0]) - 1;
    }
    printf("set charge current:%u mA\n", table[level]);
    instance.setChargeCurrent(table[level]);
    return  table[level];
#endif
#else

    const uint16_t table[] = {
        100, 125, 150, 175,
        200, 300, 400, 500,
        600, 700, 800, 900,
        1000
    };
    if (level > (sizeof(table) / sizeof(table[0]) - 1)) {
        level = sizeof(table) / sizeof(table[0]) - 1;
    }
    printf("set charge current:%u mA\n", table[level]);
    return  table[level];
#endif

}

// --- Monitor / power params -------------------------------------------

void hw_get_monitor_params(monitor_params_t &params)
{
#ifdef ARDUINO
    static monitor_params_t cached_params;
    static uint32_t last_refresh = 0;

    // Refresh at most every 1 second to save power and I2C bandwidth
    if (last_refresh != 0 && (millis() - last_refresh < 1000)) {
        params = cached_params;
        return;
    }
    last_refresh = millis();

    params = monitor_params_t{};

#if defined(USING_PPM_MANAGE)
    params.type = MONITOR_PPM;
    params.charge_state = instance.ppm.getChargeStatusString();
    params.is_charging = instance.ppm.isVbusIn(); // If VBUS is in, we consider it "charging" for the icon
    params.usb_voltage = instance.ppm.getVbusVoltage();
    params.sys_voltage = instance.ppm.getSystemVoltage();
    instance.ppm.getFaultStatus();
    if (instance.ppm.isNTCFault()) {
        params.ntc_state = instance.ppm.getNTCStatusString();
    } else {
        params.ntc_state = "Normal";
    }
#elif defined(USING_PMU_MANAGE)
    params.type = MONITOR_PMU;
    params.is_charging = instance.pmu.isCharging();
    params.charge_state = params.is_charging ? "Charging" : "Not charging";
    params.usb_voltage = instance.pmu.getVbusVoltage();
    params.sys_voltage = instance.pmu.getSystemVoltage();
    params.battery_voltage = instance.pmu.getBattVoltage();
    params.battery_percent = instance.pmu.getBatteryPercent();
    params.temperature = instance.pmu.getTemperature();
    params.ntc_state = "Normal"; //TODO:
#endif

#ifdef USING_BQ_GAUGE
    if (hw_get_device_online() & HW_GAUGE_ONLINE) {
        instance.gauge.refresh();
        params.battery_percent = instance.gauge.getStateOfCharge();
        params.battery_voltage = instance.gauge.getVoltage();
        params.instantaneousCurrent = instance.gauge.getCurrent();
        params.remainingCapacity = instance.gauge.getRemainingCapacity();
        params.fullChargeCapacity = instance.gauge.getFullChargeCapacity();
        params.standbyCurrent = instance.gauge.getStandbyCurrent();
        params.temperature = instance.gauge.getTemperature();
        params.designCapacity = instance.gauge.getDesignCapacity();
        params.averagePower = instance.gauge.getAveragePower();
        params.maxLoadCurrent = instance.gauge.getMaxLoadCurrent();
        BatteryStatus batteryStatus = instance.gauge.getBatteryStatus();

        params.is_charging = !batteryStatus.isInDischargeMode();

        if (batteryStatus.isInDischargeMode()) {
            params.timeToEmpty = instance.gauge.getTimeToEmpty();
            params.timeToFull = 0;
        } else {
            if (batteryStatus.isFullChargeDetected()) {
                params.timeToFull = 0;
                params.timeToEmpty = 0;
            } else {
                params.timeToEmpty = 0;
                params.timeToFull = instance.gauge.getTimeToFull();
            }
        }
    } else {
        // Gauge not online: Fallback to voltage-based percentage calculation
        params.battery_voltage = hw_get_battery_voltage();
        if (params.battery_voltage > 0) {
            // Simple linear mapping: 3200mV (0%) to 4200mV (100%)
            int16_t mv = params.battery_voltage;
            if (mv >= 4200) params.battery_percent = 100;
            else if (mv <= 3200) params.battery_percent = 0;
            else params.battery_percent = (mv - 3200) / 10;
        }
    }
#endif
    cached_params = params;
#else
    params.type = MONITOR_PPM;
    params.battery_percent = 30 + rand() % (100 - 30 + 1);;
    params.battery_voltage = 4178;
    params.is_charging = true;
    params.charge_state = "Fast charging";
    params.usb_voltage = 4998;
    params.ntc_state = "Normal";
#endif
}

// --- Constants ---------------------------------------------------------

uint8_t hw_get_min_charge_current()
{
    return dev_conts_var.min_charge_current;
}

uint16_t hw_get_max_charge_current()
{
    return dev_conts_var.max_charge_current;
}

uint8_t hw_get_charge_level_nums()
{
    return dev_conts_var.charge_level_nums;
}

uint8_t hw_get_charge_steps()
{
    return dev_conts_var.charge_steps;
}
