/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sleep_manager.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <cmath>

namespace sleep_manager {
namespace {

static constexpr const char* TAG = "SleepManager";

static constexpr uint32_t DISPLAY_SLEEP_TIMEOUT_MS = 30000;
static constexpr uint32_t IMU_SAMPLE_INTERVAL_MS = 100;
static constexpr uint32_t POST_SLEEP_WAKE_LOCKOUT_MS = 1200;

static constexpr float HANGING_Y_MIN = 0.70f;
static constexpr float HANGING_Z_ABS_MAX = 0.45f;

static constexpr float WAKE_Y_MAX = 0.15f;
static constexpr float WAKE_Z_MIN = 0.55f;

static constexpr uint8_t SLEEP_CONFIRM_SAMPLES = 8;
static constexpr uint8_t WAKE_CONFIRM_SAMPLES = 3;

bool s_initialized = false;
bool s_inhibit = false;
bool s_sleeping = false;

uint32_t s_last_activity_ms = 0;
uint32_t s_last_imu_sample_ms = 0;
uint32_t s_sleep_entered_ms = 0;

int s_saved_brightness = 80;

uint8_t s_sleep_orientation_count = 0;
uint8_t s_wake_orientation_count = 0;

bool readButtonActivity()
{
    GetHAL().updateButtonStates();

    return GetHAL().btnA.wasClicked() ||
           GetHAL().btnB.wasClicked() ||
           GetHAL().btnPwr.wasClicked();
}

bool readTouchActivity()
{
    return GetHAL().getTouchPoint().num > 0;
}

bool sampleImuIfDue()
{
    const uint32_t now = GetHAL().millis();
    if (s_last_imu_sample_ms != 0 && now - s_last_imu_sample_ms < IMU_SAMPLE_INTERVAL_MS) {
        return false;
    }

    s_last_imu_sample_ms = now;
    GetHAL().updateImuData();
    return true;
}

bool isHangingOrientation()
{
    const auto& imu = GetHAL().getImuData();

    return imu.accelY >= HANGING_Y_MIN &&
           std::fabs(imu.accelZ) <= HANGING_Z_ABS_MAX;
}

bool isWakeOrientation()
{
    const auto& imu = GetHAL().getImuData();

    return imu.accelY <= WAKE_Y_MAX &&
           imu.accelZ >= WAKE_Z_MIN;
}

void enterSleep()
{
    if (s_sleeping) {
        return;
    }

    s_saved_brightness = GetHAL().getBackLightBrightness();
    if (s_saved_brightness <= 0) {
        s_saved_brightness = 80;
    }

    s_sleeping = true;
    s_sleep_entered_ms = GetHAL().millis();
    s_wake_orientation_count = 0;

    mclog::tagInfo(TAG, "display sleep enter");
    GetHAL().setBackLightBrightness(0);
}

void exitSleep()
{
    if (!s_sleeping) {
        return;
    }

    s_sleeping = false;
    s_sleep_orientation_count = 0;
    s_wake_orientation_count = 0;
    s_last_activity_ms = GetHAL().millis();

    mclog::tagInfo(TAG, "display sleep wake");
    GetHAL().setBackLightBrightness(s_saved_brightness > 0 ? s_saved_brightness : 80);
}

void resetIdleState()
{
    s_last_activity_ms = GetHAL().millis();
    s_sleep_orientation_count = 0;
    s_wake_orientation_count = 0;
}

}  // namespace

void begin()
{
    if (s_initialized) {
        return;
    }

    s_initialized = true;
    resetIdleState();
    mclog::tagInfo(TAG, "begin");
}

void update()
{
    if (!s_initialized) {
        begin();
    }

    const uint32_t now = GetHAL().millis();

    if (s_inhibit) {
        if (s_sleeping) {
            exitSleep();
        }
        resetIdleState();
        return;
    }

    const bool button_activity = readButtonActivity();
    const bool touch_activity = readTouchActivity();

    if (s_sleeping) {
        if (button_activity || touch_activity) {
            exitSleep();
            return;
        }

        if (now - s_sleep_entered_ms < POST_SLEEP_WAKE_LOCKOUT_MS) {
            return;
        }

        if (sampleImuIfDue()) {
            if (isWakeOrientation()) {
                if (s_wake_orientation_count < WAKE_CONFIRM_SAMPLES) {
                    ++s_wake_orientation_count;
                }
            } else {
                s_wake_orientation_count = 0;
            }

            if (s_wake_orientation_count >= WAKE_CONFIRM_SAMPLES) {
                exitSleep();
            }
        }

        return;
    }

    if (button_activity || touch_activity) {
        markActivity();
        return;
    }

    if (now - s_last_activity_ms < DISPLAY_SLEEP_TIMEOUT_MS) {
        return;
    }

    if (sampleImuIfDue()) {
        if (isHangingOrientation()) {
            if (s_sleep_orientation_count < SLEEP_CONFIRM_SAMPLES) {
                ++s_sleep_orientation_count;
            }
        } else {
            s_sleep_orientation_count = 0;
            s_last_activity_ms = now;
        }

        if (s_sleep_orientation_count >= SLEEP_CONFIRM_SAMPLES) {
            enterSleep();
        }
    }
}

void setInhibit(bool inhibit)
{
    s_inhibit = inhibit;
    if (s_inhibit && s_sleeping) {
        exitSleep();
    }
    if (s_inhibit) {
        resetIdleState();
    }
}

bool isInhibited()
{
    return s_inhibit;
}

bool isSleeping()
{
    return s_sleeping;
}

void markActivity()
{
    if (s_sleeping) {
        exitSleep();
        return;
    }

    s_last_activity_ms = GetHAL().millis();
    s_sleep_orientation_count = 0;
    s_wake_orientation_count = 0;
}

void wake()
{
    exitSleep();
}

}  // namespace sleep_manager
