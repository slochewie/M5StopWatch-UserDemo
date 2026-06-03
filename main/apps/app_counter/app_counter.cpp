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
#include <cstdio>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

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

    LvglLockGuard lock;
    createUi();
    refreshValue();
    refreshStatus();
}

void AppCounter::onRunning()
{
    if (!_key_manager) {
        return;
    }

    auto event = _key_manager->update();
    if (event == input::KeyEvent::GoHome) {
        close();
        return;
    }
    if (event == input::KeyEvent::GoPrevious) {
        decrement();
    } else if (event == input::KeyEvent::GoNext) {
        increment();
    }

    const uint32_t now = GetHAL().millis();
    if (now - _last_status_update > 1000) {
        LvglLockGuard lock;
        refreshStatus();
    }
}

void AppCounter::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    _key_manager.reset();

    LvglLockGuard lock;
    destroyUi();
}

void AppCounter::increment()
{
    ++_count;
    LvglLockGuard lock;
    refreshValue();
}

void AppCounter::decrement()
{
    if (_count > 0) {
        --_count;
    }
    LvglLockGuard lock;
    refreshValue();
}

void AppCounter::reset()
{
    _count = 0;
    LvglLockGuard lock;
    refreshValue();
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
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "Bat %u%%   MQTT --", static_cast<unsigned>(GetHAL().getBatteryLevel()));
    lv_label_set_text(_label_status, buffer);
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

    _label_value = lv_label_create(_panel);
    lv_label_set_text(_label_value, "0");
    lv_obj_set_style_text_color(_label_value, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_label_value, &lv_font_montserrat_48, 0);
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
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0xBFBFBF), 0);
    lv_obj_set_style_text_font(_label_status, &lv_font_montserrat_18, 0);
    lv_obj_align(_label_status, LV_ALIGN_BOTTOM_MID, 0, -34);
}

void AppCounter::destroyUi()
{
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
        app->reset();
    }
}
