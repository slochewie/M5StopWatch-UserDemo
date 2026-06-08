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
#include <counter_mqtt.h>
#include <cstdlib>
#include <ctime>

using namespace mooncake;
using namespace smooth_ui_toolkit;

namespace {

void setLocalTimezone()
{
    // POSIX timezone for America/Los_Angeles:
    // UTC-8 standard time, UTC-7 daylight time,
    // DST starts second Sunday in March and ends first Sunday in November.
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
}

}  // namespace

extern "C" void app_main(void)
{
    setLocalTimezone();

    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    GetHAL().init();

    // Start MQTT immediately at boot so the retained time authority topic can
    // correct the launcher/App Setup clock without opening the Counter app.
    counter_mqtt::begin();

    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    GetMooncake().installApp(std::make_unique<AppLauncher>());
    GetMooncake().installApp(std::make_unique<AppCounter>());
    GetMooncake().installApp(std::make_unique<AppConfigure>());
    GetMooncake().installApp(std::make_unique<AppSetup>());

    while (1) {
        GetHAL().feedTheDog();
        GetMooncake().update();
    }
}
