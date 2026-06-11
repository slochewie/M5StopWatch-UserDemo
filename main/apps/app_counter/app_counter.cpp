/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_counter.h"
#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include <esp_sleep.h>
#include <cstdio>
#include <counter_service.h>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

namespace {
static constexpr uint32_t BATTERY_PUBLISH_INTERVAL_MS = 60000;
static constexpr uint32_t TIME_REFRESH_INTERVAL_MS = 1000;
static constexpr uint32_t DISPLAY_SLEEP_TIMEOUT_MS = 0;  // Phase 1: disabled; system sleep manager will replace app-owned sleep
static constexpr uint32_t IMU_WAKE_SAMPLE_INTERVAL_MS = 100;
static constexpr uint64_t PHASE2_LIGHT_SLEEP_INTERVAL_US = IMU_WAKE_SAMPLE_INTERVAL_MS * 1000ULL;
static constexpr float WAKE_ACCEL_Y_THRESHOLD = 0.30f;
static constexpr float WAKE_ACCEL_Z_THRESHOLD = 0.35f;
static constexpr uint8_t WAKE_CONFIRM_SAMPLES = 3;
static constexpr uint32_t NETWORK_WAKE_RECOVERY_DELAY_MS = 2000;

bool s_network_recover_pending = false;
uint32_t s_network_recover_after_ms = 0;

void enterPhase2LightSleepTick()
{
    esp_sleep_enable_timer_wakeup(PHASE2_LIGHT_SLEEP_INTERVAL_US);
    (void)esp_light_sleep_start();
}
}

AppCounter::AppCounter()
{
    setAppInfo().name = "Counter";
    setAppInfo().icon = (void*)&icon_counter;
}

void AppCounter::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppCounter::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    _key_manager = std::make_unique<input::KeyManager>();
    _reset_requested = false;
    _last_status_update = 0;
    _last_time_update = 0;
    _last_battery_publish = 0;
    _last_activity_ms = GetHAL().millis();
    _last_wake_sample_ms = 0;
    _saved_brightness = GetHAL().getBackLightBrightness();
    _last_published_battery = 255;
    _wake_sample_count = 0;
    _sleeping = false;
    s_network_recover_pending = false;
    s_network_recover_after_ms = 0;

    {
        LvglLockGuard lock;
        createUi();
        refreshTime(true);
        refreshValue();
        refreshStatus();
    }

    counter_service::begin();
    publishBatteryIfNeeded(true);
}

void AppCounter::onRunning()
{
    if (!_key_manager) {
        return;
    }

    const uint32_t now = GetHAL().millis();

    if (_sleeping) {
        GetHAL().updateButtonStates();
        const bool button_wake = GetHAL().btnA.wasClicked() || GetHAL().btnB.wasClicked();
        const bool touch_wake = hasTouchInput();
        const bool orientation_wake = updateOrientationWake();

        // Do not perform MQTT sync or publish work while display/light sleep is active.
        // Long sleep periods can leave the Wi-Fi/MQTT stack stale; recover after wake instead.
        if (button_wake || touch_wake || orientation_wake) {
            wakeFromDisplaySleep();
            LvglLockGuard lock;
            refreshTime(true);
            refreshValue();
            refreshStatus();
            return;
        }

        enterPhase2LightSleepTick();
        return;
    }

    if (s_network_recover_pending &&
        static_cast<int32_t>(now - s_network_recover_after_ms) >= 0) {
        s_network_recover_pending = false;
        counter_service::recoverConnection();
    }

    syncLatestMqttValue(true);

    if (_reset_requested) {
        _reset_requested = false;
        reset();
    }

    auto event = _key_manager->update();
    if (event != input::KeyEvent::None) {
        markActivity();
    }

    if (event == input::KeyEvent::GoHome) {
        close();
        return;
    }
    if (event == input::KeyEvent::GoPrevious) {
        decrement();
    } else if (event == input::KeyEvent::GoNext) {
        increment();
    }

    publishBatteryIfNeeded();

    if (now - _last_time_update > TIME_REFRESH_INTERVAL_MS) {
        LvglLockGuard lock;
        refreshTime();
    }

    if (now - _last_status_update > 1000) {
        LvglLockGuard lock;
        refreshStatus();
    }

    if (DISPLAY_SLEEP_TIMEOUT_MS > 0 &&
        now - _last_activity_ms >= DISPLAY_SLEEP_TIMEOUT_MS) {
        enterDisplaySleep();
    }
}

void AppCounter::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    if (_sleeping) {
        wakeFromDisplaySleep();
    }
    _key_manager.reset();
    _reset_requested = false;

    LvglLockGuard lock;
    destroyUi();
}

bool AppCounter::syncLatestMqttValue(bool refresh_ui)
{
    int32_t mqtt_value = 0;
    if (!counter_service::takeLatestValue(mqtt_value)) {
        return false;
    }

    _count = mqtt_value;

    if (refresh_ui) {
        LvglLockGuard lock;
        refreshValue();
        refreshStatus();
    }

    return true;
}

void AppCounter::increment()
{
    syncLatestMqttValue(false);

    ++_count;
    (void)counter_service::publishValue(_count);
    LvglLockGuard lock;
    refreshValue();
}

void AppCounter::decrement()
{
    syncLatestMqttValue(false);

    if (_count > 0) {
        --_count;
    }
    (void)counter_service::publishValue(_count);
    LvglLockGuard lock;
    refreshValue();
}

void AppCounter::reset()
{
    _count = 0;
    (void)counter_service::publishValue(_count);
    LvglLockGuard lock;
    refreshValue();
}

void AppCounter::markActivity()
{
    _last_activity_ms = GetHAL().millis();
}

void AppCounter::enterDisplaySleep()
{
    if (_sleeping) {
        return;
    }

    _sleeping = true;
    _wake_sample_count = 0;
    _last_wake_sample_ms = 0;
    _saved_brightness = GetHAL().getBackLightBrightness();
    if (_saved_brightness <= 0) {
        _saved_brightness = 80;
    }

    mclog::tagInfo(getAppInfo().name, "enter phase 2 display/light sleep");
    GetHAL().setBackLightBrightness(0);
}

void AppCounter::wakeFromDisplaySleep()
{
    if (!_sleeping) {
        return;
    }

    mclog::tagInfo(getAppInfo().name, "wake from phase 2 display/light sleep");
    _sleeping = false;
    _wake_sample_count = 0;
    GetHAL().setBackLightBrightness(_saved_brightness > 0 ? _saved_brightness : 80);
    s_network_recover_pending = true;
    s_network_recover_after_ms = GetHAL().millis() + NETWORK_WAKE_RECOVERY_DELAY_MS;
    markActivity();
}

bool AppCounter::updateOrientationWake()
{
    const uint32_t now = GetHAL().millis();
    if (_last_wake_sample_ms != 0 && now - _last_wake_sample_ms < IMU_WAKE_SAMPLE_INTERVAL_MS) {
        return false;
    }
    _last_wake_sample_ms = now;

    GetHAL().updateImuData();
    const auto& imu = GetHAL().getImuData();

    if (imu.accelY < WAKE_ACCEL_Y_THRESHOLD && imu.accelZ > WAKE_ACCEL_Z_THRESHOLD) {
        if (_wake_sample_count < WAKE_CONFIRM_SAMPLES) {
            ++_wake_sample_count;
        }
    } else {
        _wake_sample_count = 0;
    }

    return _wake_sample_count >= WAKE_CONFIRM_SAMPLES;
}

bool AppCounter::hasTouchInput()
{
    return GetHAL().getTouchPoint().num > 0;
}

void AppCounter::refreshTime(bool force)
{
    if (!_arc_top_clock) {
        return;
    }

    _arc_top_clock->update(force);
    _last_time_update = GetHAL().millis();
}

void AppCounter::refreshValue()
{
    if (!_label_value) {
        return;
    }

    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%ld", static_cast<long>(_count));
    lv_label_set_text(_label_value, buffer);
}

void AppCounter::refreshStatus()
{
    if (!_label_status) {
        return;
    }

    _last_status_update = GetHAL().millis();
    const char* topic = counter_service::counterTopic();
    if (topic == nullptr || topic[0] == '\0') {
        topic = "topic not set";
    }

    char buffer[128];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "Bat %u%%   %s\n%s",
                  static_cast<unsigned>(GetHAL().getBatteryLevel()),
                  counter_service::statusText(),
                  topic);
    lv_label_set_text(_label_status, buffer);
}

void AppCounter::publishBatteryIfNeeded(bool force)
{
    const uint32_t now = GetHAL().millis();
    uint8_t battery = GetHAL().getBatteryLevel();
    if (battery > 100) {
        battery = 100;
    }

    const bool changed = battery != _last_published_battery;
    const bool due = _last_battery_publish == 0 ||
                     now - _last_battery_publish >= BATTERY_PUBLISH_INTERVAL_MS;

    if (!force && !changed && !due) {
        return;
    }

    if (counter_service::publishBatteryPercentage(battery)) {
        _last_published_battery = battery;
        _last_battery_publish = now;
    }
}

void AppCounter::createUi()
{
    lv_obj_t* screen = lv_screen_active();
    lv_obj_clean(screen);

    _panel = lv_obj_create(screen);
    lv_obj_remove_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(_panel, 466, 466);
    lv_obj_center(_panel);
    lv_obj_set_style_bg_color(_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_panel, 0, 0);
    lv_obj_set_style_radius(_panel, 0, 0);
    lv_obj_set_style_pad_all(_panel, 0, 0);

    _arc_top_clock = std::make_unique<view::ArcTopClock>(_panel);
    _arc_top_clock->color = 0xBFBFBF;
    _arc_top_clock->displayRadius = 233;
    _arc_top_clock->updateInterval = TIME_REFRESH_INTERVAL_MS;
    _arc_top_clock->init();
    lv_obj_align(_arc_top_clock->get(), LV_ALIGN_TOP_MID, 0, 0);

    _label_value = lv_label_create(_panel);
    lv_label_set_text(_label_value, "0");
    lv_obj_set_style_text_color(_label_value, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_label_value, &lv_font_maple_mono_medium_48, 0);
    lv_obj_align(_label_value, LV_ALIGN_CENTER, 0, -35);

    _button_reset = lv_button_create(_panel);
    lv_obj_set_size(_button_reset, 210, 72);
    lv_obj_align(_button_reset, LV_ALIGN_CENTER, 0, 70);
    lv_obj_set_style_radius(_button_reset, 18, 0);
    lv_obj_add_event_cb(_button_reset, AppCounter::handleResetClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* reset_label = lv_label_create(_button_reset);
    lv_label_set_text(reset_label, "RESET");
    lv_obj_set_style_text_font(reset_label, &lv_font_montserrat_24, 0);
    lv_obj_center(reset_label);

    _label_status = lv_label_create(_panel);
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0xBFBFBF), 0);
    lv_obj_set_style_text_font(_label_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(_label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_label_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_label_status, 360);
    lv_obj_align(_label_status, LV_ALIGN_BOTTOM_MID, 0, -28);
}

void AppCounter::destroyUi()
{
    _arc_top_clock.reset();
    _label_value = nullptr;
    _label_status = nullptr;
    _button_reset = nullptr;

    if (_panel) {
        lv_obj_delete(_panel);
        _panel = nullptr;
    }
}

void AppCounter::handleResetClicked(lv_event_t* event)
{
    auto* app = static_cast<AppCounter*>(lv_event_get_user_data(event));
    if (app) {
        app->markActivity();
        app->_reset_requested = true;
    }
}
