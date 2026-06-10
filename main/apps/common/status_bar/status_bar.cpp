/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "status_bar.h"

#include <counter_service.h>
#include <hal/hal.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include <esp_err.h>
#include <esp_netif.h>
#include <esp_netif_ip_addr.h>
#include <esp_wifi.h>
#include <lvgl.h>

namespace view {
namespace {

static constexpr uint32_t COLOR_CONNECTED = 0x19C25F;
static constexpr uint32_t COLOR_DISCONNECTED = 0xD94141;

lv_obj_t* s_panel = nullptr;
lv_obj_t* s_label_device = nullptr;
lv_obj_t* s_label_ip = nullptr;
lv_obj_t* s_label_wifi = nullptr;
lv_obj_t* s_label_mqtt = nullptr;
lv_obj_t* s_battery_bar = nullptr;
lv_obj_t* s_label_charge = nullptr;
uint32_t s_color_secondary = 0xEDF4FF;
uint32_t s_color_primary = 0x385179;

bool isWifiConnected()
{
    wifi_ap_record_t ap_info = {};
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

std::string localIpAddress()
{
    if (!isWifiConnected()) {
        return "IP --";
    }

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

const char* deviceName()
{
    const char* configured_name = counter_service::deviceName();
    if (configured_name == nullptr || configured_name[0] == '\0') {
        return "M5StopWatch";
    }
    return configured_name;
}

void setTextColor(lv_obj_t* label, uint32_t color)
{
    if (label != nullptr) {
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    }
}

void setLabelText(lv_obj_t* label, const char* text)
{
    if (label != nullptr) {
        lv_label_set_text(label, text == nullptr ? "" : text);
    }
}

}  // namespace

void create_status_bar(uint32_t colorSecondary, uint32_t colorPrimary, bool silent, lv_obj_t* parent)
{
    (void)silent;

    if (s_panel != nullptr) {
        update_status_bar();
        return;
    }

    s_color_secondary = colorSecondary;
    s_color_primary = colorPrimary;

    if (parent == nullptr) {
        parent = lv_screen_active();
    }

    s_panel = lv_obj_create(parent);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_panel, 466, 64);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(s_color_secondary), 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);

    s_label_device = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_label_device, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_label_device, lv_color_hex(s_color_primary), 0);
    lv_obj_align(s_label_device, LV_ALIGN_TOP_MID, 0, 8);

    s_label_ip = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_label_ip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_ip, lv_color_hex(s_color_primary), 0);
    lv_obj_align(s_label_ip, LV_ALIGN_TOP_MID, 0, 30);

    s_label_wifi = lv_label_create(s_panel);
    lv_label_set_text(s_label_wifi, "WiFi");
    lv_obj_set_style_text_font(s_label_wifi, &lv_font_montserrat_14, 0);
    lv_obj_align(s_label_wifi, LV_ALIGN_LEFT_MID, 22, 14);

    s_label_mqtt = lv_label_create(s_panel);
    lv_label_set_text(s_label_mqtt, "MQTT");
    lv_obj_set_style_text_font(s_label_mqtt, &lv_font_montserrat_14, 0);
    lv_obj_align(s_label_mqtt, LV_ALIGN_LEFT_MID, 78, 14);

    s_battery_bar = lv_bar_create(s_panel);
    lv_obj_set_size(s_battery_bar, 34, 12);
    lv_obj_align(s_battery_bar, LV_ALIGN_RIGHT_MID, -30, 15);
    lv_bar_set_range(s_battery_bar, 0, 100);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(s_color_secondary), 0);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(s_color_primary), LV_PART_INDICATOR);
    lv_obj_set_style_outline_width(s_battery_bar, 1, 0);
    lv_obj_set_style_outline_color(s_battery_bar, lv_color_hex(s_color_primary), 0);
    lv_obj_set_style_radius(s_battery_bar, 4, 0);
    lv_obj_set_style_radius(s_battery_bar, 3, LV_PART_INDICATOR);

    s_label_charge = lv_label_create(s_panel);
    lv_label_set_text(s_label_charge, "⚡");
    lv_obj_set_style_text_font(s_label_charge, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_charge, lv_color_hex(s_color_primary), 0);
    lv_obj_align_to(s_label_charge, s_battery_bar, LV_ALIGN_CENTER, 0, -1);
    lv_obj_add_flag(s_label_charge, LV_OBJ_FLAG_HIDDEN);

    update_status_bar();
}

void update_status_bar()
{
    if (s_panel == nullptr) {
        return;
    }

    const bool wifi_connected = isWifiConnected();
    const bool mqtt_connected = counter_service::isConnected();
    const uint8_t battery = GetHAL().getBatteryLevel() > 100 ? 100 : GetHAL().getBatteryLevel();
    const bool charging = GetHAL().isBatteryCharging();
    const std::string ip = localIpAddress();

    setLabelText(s_label_device, deviceName());
    setLabelText(s_label_ip, ip.c_str());
    setTextColor(s_label_wifi, wifi_connected ? COLOR_CONNECTED : COLOR_DISCONNECTED);
    setTextColor(s_label_mqtt, mqtt_connected ? COLOR_CONNECTED : COLOR_DISCONNECTED);

    if (s_battery_bar != nullptr) {
        lv_bar_set_value(s_battery_bar, battery, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_battery_bar,
                                  lv_color_hex(charging ? COLOR_CONNECTED : s_color_primary),
                                  LV_PART_INDICATOR);
    }

    if (s_label_charge != nullptr) {
        if (charging) {
            lv_obj_remove_flag(s_label_charge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_label_charge, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void show_status_bar()
{
    if (s_panel != nullptr) {
        lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

bool is_status_bar_hidden()
{
    return s_panel == nullptr || lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

bool is_status_bar_created()
{
    return s_panel != nullptr;
}

void destroy_status_bar()
{
    s_label_device = nullptr;
    s_label_ip = nullptr;
    s_label_wifi = nullptr;
    s_label_mqtt = nullptr;
    s_battery_bar = nullptr;
    s_label_charge = nullptr;

    if (s_panel != nullptr) {
        lv_obj_delete(s_panel);
        s_panel = nullptr;
    }
}

}  // namespace view
