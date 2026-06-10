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
#include <ctime>
#include "../../../components/counter_mqtt/counter_mqtt.h"

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

namespace {
static constexpr uint32_t BATTERY_PUBLISH_INTERVAL_MS = 60000;
static constexpr uint32_t TIME_REFRESH_INTERVAL_MS = 1000;
static constexpr uint32_t DISPLAY_SLEEP_TIMEOUT_MS = 30000;
static constexpr uint32_t IMU_WAKE_SAMPLE_INTERVAL_MS = 100;
static constexpr uint64_t PHASE2_LIGHT_SLEEP_INTERVAL_US = IMU_WAKE_SAMPLE_INTERVAL_MS * 1000ULL;
static constexpr float WAKE_ACCEL_Y_THRESHOLD = 0.30f;
static constexpr float WAKE_ACCEL_Z_THRESHOLD = 0.35f;
static constexpr uint8_t WAKE_CONFIRM_SAMPLES = 3;

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
    _diagnostics_visible = false;
    _last_status_update = 0;
    _last_time_update = 0;
    _last_battery_publish = 0;
    _last_activity_ms = GetHAL().millis();
    _last_wake_sample_ms = 0;
    _saved_brightness = GetHAL().getBackLightBrightness();
    _last_published_battery = 255;
    _wake_sample_count = 0;
    _sleeping = false;

    {
        LvglLockGuard lock;
        createUi();
        refreshTime(true);
        refreshValue();
        refreshStatus();
    }

    counter_mqtt::begin();
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

        syncLatestMqttValue(false);
        publishBatteryIfNeeded();

        if (button_wake || touch_wake || orientation_wake) {
            wakeFromDisplaySleep();
            LvglLockGuard lock;
            refreshTime(true);
            refreshValue();
            refreshStatus();
            refreshDiagnostics();
            return;
        }

        enterPhase2LightSleepTick();
        return;
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
    if (!_diagnostics_visible) {
        if (event == input::KeyEvent::GoPrevious) {
            decrement();
        } else if (event == input::KeyEvent::GoNext) {
            increment();
        }
    }

    publishBatteryIfNeeded();

    if (now - _last_time_update > TIME_REFRESH_INTERVAL_MS) {
        LvglLockGuard lock;
        refreshTime();
    }

    if (now - _last_status_update > 1000) {
        LvglLockGuard lock;
        refreshStatus();
        refreshDiagnostics();
    }

    if (now - _last_activity_ms >= DISPLAY_SLEEP_TIMEOUT_MS) {
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
    _diagnostics_visible = false;

    LvglLockGuard lock;
    destroyUi();
}

bool AppCounter::syncLatestMqttValue(bool refresh_ui)
{
    int32_t mqtt_value = 0;
    if (!counter_mqtt::takeLatestValue(mqtt_value)) {
        return false;
    }

    _count = mqtt_value;

    if (refresh_ui) {
        LvglLockGuard lock;
        refreshValue();
        refreshStatus();
        refreshDiagnostics();
    }

    return true;
}

void AppCounter::increment()
{
    syncLatestMqttValue(false);

    ++_count;
    (void)counter_mqtt::publishCounterValue(_count);
    LvglLockGuard lock;
    refreshValue();
}

void AppCounter::decrement()
{
    syncLatestMqttValue(false);

    if (_count > 0) {
        --_count;
    }
    (void)counter_mqtt::publishCounterValue(_count);
    LvglLockGuard lock;
    refreshValue();
}

void AppCounter::reset()
{
    _count = 0;
    (void)counter_mqtt::publishCounterValue(_count);
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
    if (!_label_time) {
        return;
    }

    const uint32_t now_ms = GetHAL().millis();
    if (!force && now_ms - _last_time_update < TIME_REFRESH_INTERVAL_MS) {
        return;
    }

    std::time_t now = std::time(nullptr);
    std::tm local_time = {};
    if (localtime_r(&now, &local_time) == nullptr) {
        lv_label_set_text(_label_time, "--:-- --");
        _last_time_update = now_ms;
        return;
    }

    const bool is_pm = local_time.tm_hour >= 12;
    int hour_12 = local_time.tm_hour % 12;
    if (hour_12 == 0) {
        hour_12 = 12;
    }

    char buffer[12];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%d:%02d %s",
                  hour_12,
                  local_time.tm_min,
                  is_pm ? "PM" : "AM");
    lv_label_set_text(_label_time, buffer);
    _last_time_update = now_ms;
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
    const char* topic = counter_mqtt::counterTopic();
    if (topic == nullptr || topic[0] == '\0') {
        topic = "topic not set";
    }

    char buffer[128];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "Bat %u%%   %s\n%s",
                  static_cast<unsigned>(GetHAL().getBatteryLevel()),
                  counter_mqtt::statusText(),
                  topic);
    lv_label_set_text(_label_status, buffer);
}

void AppCounter::refreshDiagnostics()
{
    if (!_diagnostics_label) {
        return;
    }

    const char* device = counter_mqtt::deviceName();
    const char* ssid = counter_mqtt::wifiSsid();
    const char* broker = counter_mqtt::brokerUri();
    const char* topic = counter_mqtt::counterTopic();
    const char* battery_topic = counter_mqtt::batteryTopic();

    if (device == nullptr || device[0] == '\0') device = "not set";
    if (ssid == nullptr || ssid[0] == '\0') ssid = "not set";
    if (broker == nullptr || broker[0] == '\0') broker = "not set";
    if (topic == nullptr || topic[0] == '\0') topic = "not set";
    if (battery_topic == nullptr || battery_topic[0] == '\0') battery_topic = "not set";

    char buffer[640];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "DIAGNOSTICS\n\n"
                  "Device:\n%s\n\n"
                  "Wi-Fi:\n%s\n\n"
                  "MQTT:\n%s\n%s\n\n"
                  "Counter Topic:\n%s\n\n"
                  "Battery Topic:\n%s",
                  device,
                  ssid,
                  counter_mqtt::statusText(),
                  broker,
                  topic,
                  battery_topic);
    lv_label_set_text(_diagnostics_label, buffer);
}

void AppCounter::showDiagnostics(bool show)
{
    _diagnostics_visible = show;
    markActivity();
    if (!_diagnostics_panel) {
        return;
    }

    if (show) {
        refreshDiagnostics();
        lv_obj_remove_flag(_diagnostics_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(_diagnostics_panel);
    } else {
        lv_obj_add_flag(_diagnostics_panel, LV_OBJ_FLAG_HIDDEN);
    }
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

    if (counter_mqtt::publishBatteryPercentage(battery)) {
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

    _label_time = lv_label_create(_panel);
    lv_label_set_text(_label_time, "--:-- --");
    lv_obj_set_style_text_color(_label_time, lv_color_hex(0xBFBFBF), 0);
    lv_obj_set_style_text_font(_label_time, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(_label_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_label_time, LV_ALIGN_TOP_MID, 0, 28);

    _label_value = lv_label_create(_panel);
    lv_label_set_text(_label_value, "0");
    lv_obj_set_style_text_color(_label_value, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_label_value, &lv_font_maple_mono_medium_48, 0);
    lv_obj_align(_label_value, LV_ALIGN_CENTER, 0, -55);

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
    lv_obj_add_flag(_label_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_label_status, AppCounter::handleStatusClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0xBFBFBF), 0);
    lv_obj_set_style_text_font(_label_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(_label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_label_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_label_status, 360);
    lv_obj_align(_label_status, LV_ALIGN_BOTTOM_MID, 0, -28);

    _diagnostics_panel = lv_obj_create(_panel);
    lv_obj_set_size(_diagnostics_panel, 390, 390);
    lv_obj_center(_diagnostics_panel);
    lv_obj_set_style_bg_color(_diagnostics_panel, lv_color_hex(0x101010), 0);
    lv_obj_set_style_border_color(_diagnostics_panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(_diagnostics_panel, 2, 0);
    lv_obj_set_style_radius(_diagnostics_panel, 22, 0);
    lv_obj_set_style_pad_all(_diagnostics_panel, 18, 0);
    lv_obj_add_flag(_diagnostics_panel, LV_OBJ_FLAG_HIDDEN);

    _diagnostics_label = lv_label_create(_diagnostics_panel);
    lv_obj_set_width(_diagnostics_label, 340);
    lv_obj_set_style_text_color(_diagnostics_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_diagnostics_label, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(_diagnostics_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(_diagnostics_label, LV_ALIGN_TOP_MID, 0, 4);

    _button_diagnostics_close = lv_button_create(_diagnostics_panel);
    lv_obj_set_size(_button_diagnostics_close, 150, 54);
    lv_obj_align(_button_diagnostics_close, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(_button_diagnostics_close, 16, 0);
    lv_obj_add_event_cb(_button_diagnostics_close, AppCounter::handleDiagnosticsCloseClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* close_label = lv_label_create(_button_diagnostics_close);
    lv_label_set_text(close_label, "CLOSE");
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_20, 0);
    lv_obj_center(close_label);
}

void AppCounter::destroyUi()
{
    _label_time = nullptr;
    _label_value = nullptr;
    _label_status = nullptr;
    _button_reset = nullptr;
    _diagnostics_label = nullptr;
    _button_diagnostics_close = nullptr;
    _diagnostics_panel = nullptr;

    if (_panel) {
        lv_obj_delete(_panel);
        _panel = nullptr;
    }
}

void AppCounter::handleResetClicked(lv_event_t* event)
{
    auto* app = static_cast<AppCounter*>(lv_event_get_user_data(event));
    if (app && !app->_diagnostics_visible) {
        app->markActivity();
        app->_reset_requested = true;
    }
}

void AppCounter::handleStatusClicked(lv_event_t* event)
{
    auto* app = static_cast<AppCounter*>(lv_event_get_user_data(event));
    if (app) {
        app->showDiagnostics(true);
    }
}

void AppCounter::handleDiagnosticsCloseClicked(lv_event_t* event)
{
    auto* app = static_cast<AppCounter*>(lv_event_get_user_data(event));
    if (app) {
        app->showDiagnostics(false);
    }
}
