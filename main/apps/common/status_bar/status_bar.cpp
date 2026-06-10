/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <mooncake.h>
#include <mooncake_log.h>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <smooth_ui_toolkit.hpp>
#include <smooth_lvgl.hpp>
#include <assets/assets.h>
#include <fmt/chrono.h>
#include <hal/hal.h>
#include <memory>
#include <string>
#include <vector>
#include <lvgl.h>
#include <src/draw/lv_image_dsc.h>

#include <counter_service.h>
#include <esp_err.h>
#include <esp_netif.h>
#include <esp_netif_ip_addr.h>
#include <esp_wifi.h>

using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

class StatuBarGesture {
public:
    std::function<void(void)> onGesture;

    StatuBarGesture() : _is_tracking(false), _last_state(LV_INDEV_STATE_REL)
    {
    }

    void init()
    {
        _is_tracking    = false;
        _last_state     = LV_INDEV_STATE_REL;
        _screen_height  = 466;
        _top_threshold  = 20;
        _swipe_min_dist = 50;
    }

    void update()
    {
        lv_indev_t* indev = GetHAL().lvTouchpad;
        if (!indev) {
            return;
        }

        lv_indev_state_t state = lv_indev_get_state(indev);
        lv_point_t curr_point;
        lv_indev_get_point(indev, &curr_point);

        if (state == LV_INDEV_STATE_PR && _last_state == LV_INDEV_STATE_REL) {
            if (curr_point.y <= _top_threshold && curr_point.y >= 0) {
                _start_point = curr_point;
                _is_tracking = true;
            } else {
                _is_tracking = false;
            }
        } else if (state == LV_INDEV_STATE_REL && _last_state == LV_INDEV_STATE_PR) {
            if (_is_tracking) {
                int delta_y = curr_point.y - _start_point.y;
                int delta_x = abs(curr_point.x - _start_point.x);
                if (delta_y > _swipe_min_dist && delta_y > delta_x) {
                    if (onGesture) {
                        onGesture();
                    }
                }
                _is_tracking = false;
            }
        }

        _last_state = state;
    }

private:
    bool _is_tracking;
    lv_indev_state_t _last_state;
    lv_point_t _start_point;
    int _screen_height;
    int _top_threshold;
    int _swipe_min_dist;
};

namespace status_bar_view {
namespace {

std::string localIpAddress()
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        return "IP --";
    }

    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return "IP --";
    }

    char buffer[24] = {};
    std::snprintf(buffer, sizeof(buffer), IPSTR, IP2STR(&ip_info.ip));
    return buffer;
}

int wifiRssi()
{
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return static_cast<int>(ap_info.rssi);
}

const char* deviceName()
{
    const char* configured_name = counter_service::deviceName();
    if (configured_name == nullptr || configured_name[0] == '\0') {
        return "M5StopWatch";
    }
    return configured_name;
}

}  // namespace

class Widget {
public:
    virtual ~Widget()     = default;
    virtual void update() = 0;
};

class StatusInfo : public Widget {
public:
    StatusInfo(lv_obj_t* parent, uint32_t colorPrimary)
    {
        _label = std::make_unique<Label>(parent);
        _label->setText("");
        _label->setTextColor(lv_color_hex(colorPrimary));
        _label->setTextFont(&lv_font_montserrat_16);
        _label->align(LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_set_style_text_align(_label->get(), LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(_label->get(), 280);
        update();
    }

    void update() override
    {
        const std::string ip = localIpAddress();
        const int rssi = wifiRssi();
        const uint8_t raw_level = GetHAL().getBatteryLevel();
        const uint8_t level = raw_level > 100 ? 100 : raw_level;
        const char* mqtt = counter_service::isConnected() ? "MQTT" : "MQTT --";

        char buffer[160] = {};
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s\n%s\n\n📶%d  ●%s     %u%% 🔋",
                      deviceName(),
                      ip.c_str(),
                      rssi,
                      mqtt,
                      static_cast<unsigned>(level));
        _label->setText(buffer);
    }

private:
    std::unique_ptr<Label> _label;
};

class StatusBarView {
public:
    StatusBarView(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        (void)parent;
        _panel = std::make_unique<uitk::lvgl_cpp::Container>(lv_screen_active());
        _panel->setBgColor(lv_color_hex(colorSecondary));
        _panel->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        _panel->align(LV_ALIGN_TOP_MID, 0, 0);
        _panel->setBorderWidth(0);
        _panel->setSize(300, 122);
        _panel->setRadius(28);
        _panel->setPadding(0, 0, 0, 0);
        _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        _panel->onClick().connect([this]() { hide(); });

        _widgets.push_back(std::make_unique<StatusInfo>(_panel->get(), colorPrimary));

        _panel->setPos(0, _pos_y_hide);
        _panel->setHidden(true);
        _is_hidden                                 = true;
        _pos_y_anim.springOptions().bounce         = 0.1;
        _pos_y_anim.springOptions().visualDuration = 0.3;
        _pos_y_anim.teleport(_pos_y_hide);
    }

    void update()
    {
        _pos_y_anim.update();
        if (!_pos_y_anim.done()) {
            _panel->setPos(0, _pos_y_anim.directValue());
        } else {
            if (_hide_panel_flag) {
                _hide_panel_flag = false;
                _panel->setHidden(true);
            }
        }

        if (GetHAL().millis() - _last_update_tick > 1000) {
            _last_update_tick = GetHAL().millis();
            for (auto& widget : _widgets) {
                widget->update();
            }
        }
    }

    void show()
    {
        _panel->moveForeground();
        _panel->setHidden(false);
        _pos_y_anim = _pos_y_show;
        _is_hidden  = false;
    }

    void hide()
    {
        _pos_y_anim      = _pos_y_hide;
        _is_hidden       = true;
        _hide_panel_flag = true;
    }

    bool isHidden() const
    {
        return _is_hidden;
    }

private:
    const int _pos_y_show = -8;
    const int _pos_y_hide = -135;

    std::unique_ptr<Container> _panel;
    std::vector<std::unique_ptr<Widget>> _widgets;
    AnimateValue _pos_y_anim;
    bool _is_hidden            = false;
    bool _hide_panel_flag      = false;
    uint32_t _last_update_tick = 0;
};

}  // namespace status_bar_view

class StatusBar {
public:
    void init(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary, bool silent)
    {
        _status_bar_gesture            = std::make_unique<StatuBarGesture>();
        _status_bar_gesture->onGesture = [&]() { handle_gesture(); };
        _status_bar_gesture->init();

        _status_bar_view = std::make_unique<status_bar_view::StatusBarView>(parent, colorSecondary, colorPrimary);
        _is_first_show   = !silent;

        if (!silent) {
            _status_bar_view->show();
            _status_bar_show_tick = GetHAL().millis();
        }
    }

    void update()
    {
        _status_bar_gesture->update();
        _status_bar_view->update();
        update_visibility();
    }

    void show()
    {
        handle_gesture();
    }

    bool isHidden() const
    {
        return _status_bar_view == nullptr ? true : _status_bar_view->isHidden();
    }

private:
    std::unique_ptr<StatuBarGesture> _status_bar_gesture;
    std::unique_ptr<status_bar_view::StatusBarView> _status_bar_view;
    uint32_t _status_bar_show_tick = 0;
    bool _is_first_show            = true;

    void handle_gesture()
    {
        _status_bar_view->show();
        _status_bar_show_tick = GetHAL().millis();
    }

    void update_visibility()
    {
        if (!_status_bar_view->isHidden()) {
            if (GetHAL().millis() - _status_bar_show_tick > (_is_first_show ? 1800 : 6000)) {
                _is_first_show = false;
                _status_bar_view->hide();
            }
        }
    }
};

namespace view {

static std::unique_ptr<StatusBar> _status_bar;

void create_status_bar(uint32_t colorSecondary, uint32_t colorPrimary, bool silent, lv_obj_t* parent)
{
    _status_bar = std::make_unique<StatusBar>();
    _status_bar->init(parent, colorSecondary, colorPrimary, silent);
}

void update_status_bar()
{
    if (_status_bar) {
        _status_bar->update();
    }
}

void show_status_bar()
{
    if (_status_bar) {
        _status_bar->show();
    }
}

bool is_status_bar_hidden()
{
    return _status_bar == nullptr ? true : _status_bar->isHidden();
}

bool is_status_bar_created()
{
    return _status_bar != nullptr;
}

void destroy_status_bar()
{
    _status_bar.reset();
}

}  // namespace view
