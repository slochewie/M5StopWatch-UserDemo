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
#include <counter_service.h>
#include <cstdlib>
#include <ctime>

using namespace mooncake;
using namespace smooth_ui_toolkit;

namespace {

static constexpr uint32_t NETWORK_RECOVERY_INTERVAL_MS = 5000;

void setLocalTimezone()
{
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
}

void runSystemNetworkTick()
{
    static uint32_t last_recovery_ms = 0;
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

    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    GetHAL().init();

    counter_service::begin();

    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    GetMooncake().installApp(std::make_unique<AppLauncher>());
    GetMooncake().installApp(std::make_unique<AppCounter>());
    GetMooncake().installApp(std::make_unique<AppConfigure>());
    GetMooncake().installApp(std::make_unique<AppSetup>());

    while (1) {
        GetHAL().feedTheDog();
        runSystemNetworkTick();
        GetMooncake().update();
    }
}
