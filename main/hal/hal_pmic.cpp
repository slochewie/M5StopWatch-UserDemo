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
#include <cstring>
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

constexpr uint32_t _bat_reading_awake_period_ms = 1000;
constexpr uint32_t _bat_reading_sleep_period_ms = 60000;
constexpr uint16_t _bat_filter_weight_old = 7;
constexpr uint16_t _bat_filter_weight_new = 1;
constexpr uint16_t _bat_filter_weight_sum = _bat_filter_weight_old + _bat_filter_weight_new;

uint8_t _bat_level        = 0;
uint16_t _bat_filtered_mv = 0;
bool _pmic_app_sleep = false;
std::mutex _bat_level_mutex;
std::mutex _pmic_sleep_mutex;

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

bool is_pmic_app_sleep()
{
    std::lock_guard<std::mutex> lock(_pmic_sleep_mutex);
    return _pmic_app_sleep;
}

void set_pmic_app_sleep(bool sleeping)
{
    std::lock_guard<std::mutex> lock(_pmic_sleep_mutex);
    _pmic_app_sleep = sleeping;
}

uint32_t current_bat_reading_period_ms()
{
    return is_pmic_app_sleep() ? _bat_reading_sleep_period_ms : _bat_reading_awake_period_ms;
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

        vTaskDelay(pdMS_TO_TICKS(current_bat_reading_period_ms()));
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
    _pm1->ldoSetPowerHold(false);

    // set charge enable or disable, this setting will keep working after power off
    _pm1->setChargeEnable(true);

    // Keep PMG0 in the factory/default PMIC configuration. Configuring PMG0 as
    // an explicit wake-function input caused power-button double-click to reboot
    // instead of powering off on tested hardware. BMI270/orientation wake remains
    // available through the app-level fallback path.

    pmicRunPmg0PublicApiProbe("enable");
    // drive CHG_PROG low to force active charge programming
    _pm1->gpioSet(PMG3_CHG_PROG, M5PM1_GPIO_MODE_OUTPUT, 0, M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL);

    _pm1->gpioSetFunc(PMG2_CHG_STAT, M5PM1_GPIO_FUNC_GPIO);
    _pm1->gpioSetMode(PMG2_CHG_STAT, M5PM1_GPIO_MODE_INPUT);
    _pm1->gpioSetPull(PMG2_CHG_STAT, M5PM1_GPIO_PULL_NONE);

    // Match the upstream StopWatch PMIC behavior: prevent accidental single-click
    // reset while leaving the PMIC's built-in double-click power-off path intact.
    _pm1->setSingleResetDisable(true);

    uint16_t battery_mv = 0;
    if (_pm1->readVbat(&battery_mv) == M5PM1_OK) {
        update_bat_level_from_mv(battery_mv);
    }

    pmicLogPmg0State("boot-after-pmic-init");

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

void Hal::pmicEnterAppSleep()
{
    set_pmic_app_sleep(true);
    mclog::tagInfo(_tag, "app sleep: battery polling slowed");
}

void Hal::pmicExitAppSleep()
{
    set_pmic_app_sleep(false);
    mclog::tagInfo(_tag, "app wake: battery polling restored");

    if (_pm1) {
        uint16_t battery_mv = 0;
        if (_pm1->readVbat(&battery_mv) == M5PM1_OK) {
            update_bat_level_from_mv(battery_mv);
        }
    }
}

bool Hal::isPmicAppSleep()
{
    return is_pmic_app_sleep();
}

bool Hal::pmicGetPmg0Level(uint8_t& level)
{
    if (!_pm1) {
        return false;
    }

    uint8_t pmg0_level = 1;
    const auto result = _pm1->gpioGetInput(PMG0_RTC_IMU_INT, &pmg0_level);
    if (result != M5PM1_OK) {
        return false;
    }

    level = pmg0_level;
    return true;
}

void Hal::pmicRunPmg0PublicApiProbe(const char* mode)
{
    if (!_pm1) {
        mclog::tagWarn(_tag, "PMG0 public API probe skipped: PMIC not initialized");
        return;
    }

    const char* selected_mode = mode ? mode : "readonly";

    uint8_t wake_src_before = 0;
    const auto wake_before_result = _pm1->getWakeSource(&wake_src_before, M5PM1_CLEAN_NONE);

    uint8_t pmg0_before = 1;
    const auto pmg0_before_result = _pm1->gpioGetInput(PMG0_RTC_IMU_INT, &pmg0_before);

    mclog::tagInfo(_tag,
                   "PMG0 public API probe before mode={}: wake_result={} WAKE_SRC=0x{:02X} pmg0_result={} PMG0_LEVEL={}",
                   selected_mode,
                   static_cast<int>(wake_before_result),
                   static_cast<int>(wake_src_before),
                   static_cast<int>(pmg0_before_result),
                   static_cast<int>(pmg0_before));

    m5pm1_err_t action_result = M5PM1_OK;

    if (std::strcmp(selected_mode, "enable") == 0) {
        action_result = _pm1->gpioSetWakeEnable(PMG0_RTC_IMU_INT, true);
        mclog::tagInfo(_tag, "PMG0 public API action: gpioSetWakeEnable(PMG0, true) -> {}",
                       static_cast<int>(action_result));
    } else if (std::strcmp(selected_mode, "edge-falling") == 0) {
        action_result = _pm1->gpioSetWakeEdge(PMG0_RTC_IMU_INT, M5PM1_GPIO_WAKE_FALLING);
        mclog::tagInfo(_tag, "PMG0 public API action: gpioSetWakeEdge(PMG0, FALLING) -> {}",
                       static_cast<int>(action_result));
    } else if (std::strcmp(selected_mode, "edge-rising") == 0) {
        action_result = _pm1->gpioSetWakeEdge(PMG0_RTC_IMU_INT, M5PM1_GPIO_WAKE_RISING);
        mclog::tagInfo(_tag, "PMG0 public API action: gpioSetWakeEdge(PMG0, RISING) -> {}",
                       static_cast<int>(action_result));
    } else if (std::strcmp(selected_mode, "restore") == 0) {
        action_result = _pm1->gpioSetWakeEnable(PMG0_RTC_IMU_INT, false);
        mclog::tagInfo(_tag, "PMG0 public API action: gpioSetWakeEnable(PMG0, false) -> {}",
                       static_cast<int>(action_result));
    } else {
        mclog::tagInfo(_tag, "PMG0 public API action: readonly, no PMG0 wake changes");
    }

    uint8_t wake_src_after = 0;
    const auto wake_after_result = _pm1->getWakeSource(&wake_src_after, M5PM1_CLEAN_NONE);

    uint8_t pmg0_after = 1;
    const auto pmg0_after_result = _pm1->gpioGetInput(PMG0_RTC_IMU_INT, &pmg0_after);

    mclog::tagInfo(_tag,
                   "PMG0 public API probe after mode={}: wake_result={} WAKE_SRC=0x{:02X} pmg0_result={} PMG0_LEVEL={}",
                   selected_mode,
                   static_cast<int>(wake_after_result),
                   static_cast<int>(wake_src_after),
                   static_cast<int>(pmg0_after_result),
                   static_cast<int>(pmg0_after));
}

void Hal::pmicLogPmg0State(const char* reason)
{
    if (!_pm1) {
        mclog::tagWarn(_tag, "PMG0 read-only diag skipped ({}): PMIC not initialized", reason ? reason : "unknown");
        return;
    }

    uint8_t pmg0_level = 1;
    const auto result = _pm1->gpioGetInput(PMG0_RTC_IMU_INT, &pmg0_level);
    if (result == M5PM1_OK) {
        mclog::tagInfo(_tag, "PMG0 read-only diag ({}): PMG0_RTC_IMU_INT level={}",
                       reason ? reason : "unknown",
                       static_cast<int>(pmg0_level));
    } else {
        mclog::tagWarn(_tag, "PMG0 read-only diag ({}) failed: {}",
                       reason ? reason : "unknown",
                       static_cast<int>(result));
    }
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
