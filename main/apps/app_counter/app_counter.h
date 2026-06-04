/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
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
    lv_obj_t* _panel = nullptr;
    lv_obj_t* _label_value = nullptr;
    lv_obj_t* _label_status = nullptr;
    lv_obj_t* _button_reset = nullptr;
    int32_t _count = 0;
    uint32_t _last_status_update = 0;
    bool _reset_requested = false;

    void increment();
    void decrement();
    void reset();
    void refreshValue();
    void refreshStatus();
    void createUi();
    void destroyUi();

    static void handleResetClicked(lv_event_t* event);
};
