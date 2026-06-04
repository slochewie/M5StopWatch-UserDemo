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

class AppConfigure : public mooncake::AppAbility {
public:
    AppConfigure();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<input::KeyManager> _key_manager;
    lv_obj_t* _panel = nullptr;
    lv_obj_t* _label_title = nullptr;
    lv_obj_t* _label_status = nullptr;
    lv_obj_t* _button_start = nullptr;
    bool _start_requested = false;
    bool _portal_active = false;
    bool _is_open = false;

    void createUi();
    void destroyUi();
    void refreshStatus(const char* message);
    void startConfigurePortal();
    void onPortalClosed();

    static void handleStartClicked(lv_event_t* event);
    static void portalTask(void* arg);
};
