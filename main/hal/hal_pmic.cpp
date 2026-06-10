/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <M5PM1.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <cmath>

static const std::string_view _tag = "HAL-PMIC";
static std::unique_ptr<M5PM1> _pm1;

namespace {

uint8_t battery_millivolts_to_percent(uint16_t millivolts)
{
    constexpr uint16_t _battery_mv_empty = 3300;
    constexpr uint16_t _battery_mv_full  = 4200;

    if (millivolts <= _battery_mv_empty) {
        return 0;
    }
    if (millivolts >= _battery_mv_full) {
        return 100;
    }

    const auto scaled =
        static_cast<uint32_t>(millivolts - _battery_mv_empty) * 100U / (_battery_mv_full - _battery_mv_empty);
    return static_cast<uint8_t>(std::min<uint32_t>(scaled, 100U));
}

constexpr uint32_t _bat_reading_period_ms = 1000;
constexpr uint16_t _bat_filter_weight_old = 7;
constexpr uint16_t _bat_filter_weight_new = 1;
constexpr uint16_t _bat_filter_weight_sum = _bat_filter_weight_old + _bat_filter_weight_new;

uint8_t _bat_level        = 0;
uint16_t _bat_filtered_mv = 0;
std::mutex _bat_level_mutex;

void update_bat_level(uint8_t level)
{
    std::lock_guard<std::mutex> lock(_bat_level_mutex);
    _bat_level = level;
}

void update_bat_level_from_mv(uint16_t millivolts)
{
    if (_bat_filtered_mv == 0) {
        _bat_filtered_mv = millivolts;
    } else {
        const uint32_t filtered = static_cast<uint32_t>(_bat_filtered_mv) * _bat_filter_weight_old +
                                  static_cast<uint32_t>(millivolts) * _bat_filter_weight_new;
        _bat_filtered_mv = static_cast<uint16_t>((filtered + (_bat_filter_weight_sum / 2)) / _bat_filter_weight_sum);
    }

    update_bat_level(battery_millivolts_to_percent(_bat_filtered_mv));
}

void bat_reading_task(void* param)
{
    mclog::tagInfo(_tag, "start bat reading task");

    while (true) {
        if (_pm1) {
            uint16_t battery_mv = 0;
            if (_pm1->readVbat(&battery_mv) == M5PM1_OK) {
                update_bat_level_from_mv(battery_mv);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(_bat_reading_period_ms));
    }
}

void log_pmic_result(const char* action, m5pm1_err_t result)
{
    if (result == M5PM1_OK) {
        mclog::tagInfo(_tag, "{} ok", action);
    } else {
        mclog::tagWarn(_tag, "{} failed: {}", action, static_cast<int>(result));
    }
}

}  // namespace

// PMIC IO
#define PMG0_RTC_IMU_INT M5PM1_GPIO_NUM_0
#define PMG2_CHG_STAT    M5PM1_GPIO_NUM_2
#define PMG4_PORT_INT    M5PM1_GPIO_NUM_4
#define PMG3_CHG_PROG    M5PM1_GPIO_NUM_3
#define PMG1_G12_PY_IRQ  M5PM1_GPIO_NUM_1

void Hal::pmic_init()
{
    mclog::tagInfo(_tag, "pmic init");

    _pm1 = std::make_unique<M5PM1>();
    if (_pm1->begin(i2c_bus_get_internal_bus_handle(_i2c_bus)) != M5PM1_OK) {
        mclog::tagInfo(_tag, "init failed");
        _pm1.reset();
        return;
    }

    _pm1->setI2cSleepTime(0);
    _pm1->setI2cSleepTime(0);

    // set button delay click 1s
    _pm1->btnSetConfig(M5PM1_BTN_TYPE_CLICK, M5PM1_BTN_CLICK_DELAY_1000MS);
    // disable WDT, default is open
    _pm1->wdtSet(0);
    //  hold LDO power close when power off, keep power for RTC
    _pm1->ldoSetPowerHold(true);

    // set charge enable or disable, this setting will keep working after power off
    _pm1->setChargeEnable(true);

    // The StopWatch schematic routes BMI270 INT1 and RTC INT through PMG0_RTC&IMU_INT
    // into M5PM1 GPIO0. Configure GPIO0 as a falling-edge wake input, but do not
    // power down rails here; Phase 2 still uses timer light sleep as the safe fallback.
    log_pmic_result("PMG0 wake input mode", _pm1->gpioSetMode(PMG0_RTC_IMU_INT, M5PM1_GPIO_MODE_INPUT));
    log_pmic_result("PMG0 wake pull", _pm1->gpioSetPull(PMG0_RTC_IMU_INT, M5PM1_GPIO_PULL_UP));
    log_pmic_result("PMG0 wake edge", _pm1->gpioSetWakeEdge(PMG0_RTC_IMU_INT, M5PM1_GPIO_WAKE_FALLING));
    log_pmic_result("PMG0 wake enable", _pm1->gpioSetWakeEnable(PMG0_RTC_IMU_INT, true));
    log_pmic_result("PMG0 wake func", _pm1->gpioSetFunc(PMG0_RTC_IMU_INT, M5PM1_GPIO_FUNC_WAKE));
    log_pmic_result("PMIC wake source clear", _pm1->clearWakeSource(M5PM1_WAKE_SRC_EXT_WAKE));

    // drive CHG_PROG low to force active charge programming
    _pm1->gpioSet(PMG3_CHG_PROG, M5PM1_GPIO_MODE_OUTPUT, 0, M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL);

    _pm1->gpioSetFunc(PMG2_CHG_STAT, M5PM1_GPIO_FUNC_GPIO);
    _pm1->gpioSetMode(PMG2_CHG_STAT, M5PM1_GPIO_MODE_INPUT);
    _pm1->gpioSetPull(PMG2_CHG_STAT, M5PM1_GPIO_PULL_NONE);

    // Disable single-click reset so the first click of a double-click does not reboot.
    // Explicitly enable double-click power-off to restore the factory power-button behavior.
    log_pmic_result("PMIC single-click reset disable", _pm1->setSingleResetDisable(true));
    log_pmic_result("PMIC double-click power-off enable", _pm1->setDoubleOffDisable(false));

    uint16_t battery_mv = 0;
    if (_pm1->readVbat(&battery_mv) == M5PM1_OK) {
        update_bat_level_from_mv(battery_mv);
    }

    xTaskCreate(bat_reading_task, "bat_reading", 4 * 1024, NULL, 1, NULL);
}

bool Hal::pmic_get_pwr_btn_state()
{
    bool result = false;
    if (_pm1) {
        return _pm1->btnGetState(&result);
    }
    return result;
}

uint8_t Hal::getBatteryLevel()
{
    std::lock_guard<std::mutex> lock(_bat_level_mutex);
    return _bat_level;
}

bool Hal::isBatteryCharging(bool strict)
{
    if (!_pm1) {
        return false;
    }

    uint16_t vin_mv       = 0;
    uint8_t charge_status = 1;

    const auto vin_result = _pm1->readVin(&vin_mv);
    const auto chg_result = _pm1->gpioGetInput(PMG2_CHG_STAT, &charge_status);
    if (vin_result != M5PM1_OK || chg_result != M5PM1_OK) {
        return false;
    }

    const bool external_power_inserted = vin_mv > 4000;
    const bool charging_active         = charge_status == 0;

    if (strict) {
        return external_power_inserted && charging_active;
    }
    return external_power_inserted;
}
