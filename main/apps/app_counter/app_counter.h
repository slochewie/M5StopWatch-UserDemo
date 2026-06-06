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

    // Phase 5 diagnostics overlay
    lv_obj_t* _diagnostics_panel = nullptr;
    lv_obj_t* _diagnostics_label = nullptr;
    lv_obj_t* _button_diagnostics_close = nullptr;
    bool _diagnostics_visible = false;

    int32_t _count = 0;
    uint32_t _last_status_update = 0;
    bool _reset_requested = false;

    bool syncLatestMqttValue(bool refresh_ui);
    void increment();
    void decrement();
    void reset();
    void refreshValue();
    void refreshStatus();
    void refreshDiagnostics();
    void showDiagnostics(bool show);
    void createUi();
    void destroyUi();

    static void handleResetClicked(lv_event_t* event);
    static void handleStatusClicked(lv_event_t* event);
    static void handleDiagnosticsCloseClicked(lv_event_t* event);
};
