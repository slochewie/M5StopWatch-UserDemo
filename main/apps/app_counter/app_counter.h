/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <apps/common/arc_top_clock/arc_top_clock.h>
#include <apps/common/key_manager/key_manager.h>
#include <mooncake.h>
#include <memory>
#include <cstdint>

class AppCounter : public mooncake::AppAbility {
public:
    AppCounter();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<input::KeyManager> _key_manager;
    std::unique_ptr<view::ArcTopClock> _arc_top_clock;
    lv_obj_t* _panel = nullptr;
    lv_obj_t* _label_value = nullptr;
    lv_obj_t* _label_status = nullptr;
    lv_obj_t* _button_reset = nullptr;

    int32_t _count = 0;
    uint32_t _last_status_update = 0;
    uint32_t _last_time_update = 0;
    uint32_t _last_battery_publish = 0;
    uint32_t _last_activity_ms = 0;
    uint32_t _last_wake_sample_ms = 0;
    int _saved_brightness = 80;
    uint8_t _last_published_battery = 255;
    uint8_t _wake_sample_count = 0;
    bool _reset_requested = false;
    bool _sleeping = false;

    bool syncLatestMqttValue(bool refresh_ui);
    void increment();
    void decrement();
    void reset();
    void markActivity();
    void enterDisplaySleep();
    void wakeFromDisplaySleep();
    bool updateOrientationWake();
    bool hasTouchInput();
    void refreshTime(bool force = false);
    void refreshValue();
    void refreshStatus();
    void publishBatteryIfNeeded(bool force = false);
    void createUi();
    void destroyUi();

    static void handleResetClicked(lv_event_t* event);
};
