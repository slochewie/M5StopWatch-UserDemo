/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_configure.h"

#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>

using namespace mooncake;

AppConfigure::AppConfigure()
{
    setAppInfo().name = "Configure";
    // Reuse the setup icon for Phase 1. A dedicated configure icon can be added later.
    setAppInfo().icon = (void*)&icon_setup;
}

void AppConfigure::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppConfigure::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    _key_manager = std::make_unique<input::KeyManager>();
    _start_requested = false;

    LvglLockGuard lock;
    createUi();
    refreshStatus("Config portal scaffold\nPhase 2 will start AP mode");
}

void AppConfigure::onRunning()
{
    if (_key_manager) {
        auto event = _key_manager->update();
        if (event == input::KeyEvent::GoHome) {
            close();
            return;
        }
    }

    if (_start_requested) {
        _start_requested = false;
        startConfigurePortal();
    }
}

void AppConfigure::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    _key_manager.reset();
    _start_requested = false;

    LvglLockGuard lock;
    destroyUi();
}

void AppConfigure::createUi()
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

    _label_title = lv_label_create(_panel);
    lv_label_set_text(_label_title, "Configure");
    lv_obj_set_style_text_color(_label_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_label_title, &lv_font_montserrat_28, 0);
    lv_obj_align(_label_title, LV_ALIGN_CENTER, 0, -135);

    _label_status = lv_label_create(_panel);
    lv_obj_set_width(_label_status, 330);
    lv_label_set_long_mode(_label_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0xBFBFBF), 0);
    lv_obj_set_style_text_font(_label_status, &lv_font_montserrat_18, 0);
    lv_obj_align(_label_status, LV_ALIGN_CENTER, 0, -30);

    _button_start = lv_button_create(_panel);
    lv_obj_set_size(_button_start, 230, 72);
    lv_obj_align(_button_start, LV_ALIGN_CENTER, 0, 95);
    lv_obj_set_style_radius(_button_start, 18, 0);
    lv_obj_add_event_cb(_button_start, AppConfigure::handleStartClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* start_label = lv_label_create(_button_start);
    lv_label_set_text(start_label, "START PORTAL");
    lv_obj_set_style_text_font(start_label, &lv_font_montserrat_22, 0);
    lv_obj_center(start_label);
}

void AppConfigure::destroyUi()
{
    _label_title = nullptr;
    _label_status = nullptr;
    _button_start = nullptr;

    if (_panel) {
        lv_obj_delete(_panel);
        _panel = nullptr;
    }
}

void AppConfigure::refreshStatus(const char* message)
{
    if (_label_status) {
        lv_label_set_text(_label_status, message ? message : "");
    }
}

void AppConfigure::startConfigurePortal()
{
    LvglLockGuard lock;
    refreshStatus("Portal not implemented yet.\nNext phase adds AP + web form.");
}

void AppConfigure::handleStartClicked(lv_event_t* event)
{
    auto* app = static_cast<AppConfigure*>(lv_event_get_user_data(event));
    if (app) {
        app->_start_requested = true;
    }
}
