/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <lv_demos.h>
#include <apps/common/audio/audio.h>
#include <apps/common/sleep_manager/sleep_manager.h>
#include <counter_service.h>
#include <hal/utils/configure_ap/configure_ap.h>
#include <cstdlib>
#include <ctime>
#include <esp_pm.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

namespace {

static constexpr uint32_t NETWORK_RECOVERY_INTERVAL_MS = 5000;

void configureCpuPowerManagement()
{
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };

    const esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        mclog::tagInfo("Power", "CPU frequency scaling enabled: 80-240 MHz");
    } else {
        mclog::tagWarn("Power", "CPU frequency scaling setup failed: {}", static_cast<int>(err));
    }
}

void setLocalTimezone()
{
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
}

void runSystemNetworkTick()
{
    static uint32_t last_recovery_ms = 0;

    if (configure_ap::isRunning()) {
        last_recovery_ms = GetHAL().millis();
        return;
    }

    const uint32_t now = GetHAL().millis();

    if (last_recovery_ms == 0 || now - last_recovery_ms >= NETWORK_RECOVERY_INTERVAL_MS) {
        last_recovery_ms = now;
        counter_service::recoverConnection();
    }
}

}  // namespace

extern "C" void app_main(void)
{
    setLocalTimezone();
    configureCpuPowerManagement();

    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    GetHAL().init();

    counter_service::begin();
    sleep_manager::begin();

    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    GetMooncake().installApp(std::make_unique<AppLauncher>());
    GetMooncake().installApp(std::make_unique<AppCounter>());
    GetMooncake().installApp(std::make_unique<AppConfigure>());
    GetMooncake().installApp(std::make_unique<AppSetup>());

    while (1) {
        GetHAL().feedTheDog();
        runSystemNetworkTick();
        sleep_manager::setInhibit(configure_ap::isRunning());

        if (sleep_manager::isSleeping()) {
            // While asleep, sleep_manager owns wake inputs.
            sleep_manager::update();
        } else {
            // While awake, apps own button click events first.
            GetMooncake().update();
            sleep_manager::update();
        }
    }
}
