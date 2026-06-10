#include "wifi_service.h"

#include <cstring>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <nvs_flash.h>

namespace common::wifi {
namespace {

static constexpr const char* TAG = "WifiService";
static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr uint32_t WIFI_CONNECT_WAIT_MS = 15000;

Config s_config;
EventGroupHandle_t s_event_group = nullptr;
esp_netif_t* s_netif = nullptr;
bool s_netif_ready = false;
bool s_handlers_registered = false;
bool s_initialized = false;
bool s_started = false;
bool s_connected = false;
int s_retry_count = 0;

bool ensureNetworkStackReady()
{
    if (s_netif_ready) {
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init requested erase: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return false;
    }

    s_netif_ready = true;
    ESP_LOGI(TAG, "Network stack ready");
    return true;
}

void eventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_started = true;
        s_connected = false;
        ESP_LOGI(TAG, "Wi-Fi STA started");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_event_group != nullptr) {
            xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        }

        ++s_retry_count;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnect attempt %d", s_retry_count);
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        s_retry_count = 0;
        s_connected = true;
        ESP_LOGI(TAG, "Wi-Fi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_event_group != nullptr) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
        return;
    }
}

bool ensureInitialized()
{
    if (s_initialized) {
        return true;
    }

    if (!ensureNetworkStackReady()) {
        return false;
    }

    if (s_event_group == nullptr) {
        s_event_group = xEventGroupCreate();
        if (s_event_group == nullptr) {
            ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
            return false;
        }
    }

    if (s_netif == nullptr) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (s_netif == nullptr) {
            ESP_LOGE(TAG, "Failed to create default Wi-Fi STA netif");
            return false;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!s_handlers_registered) {
        esp_event_handler_instance_t wifi_any_id = nullptr;
        esp_event_handler_instance_t got_ip = nullptr;
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr, &wifi_any_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register WIFI_EVENT handler failed: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, nullptr, &got_ip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register IP_EVENT handler failed: %s", esp_err_to_name(err));
            return false;
        }
        s_handlers_registered = true;
    }

    s_initialized = true;
    return true;
}

bool applyConfig()
{
    if (s_config.ssid.empty()) {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty. Open Configure and save Wi-Fi settings.");
        return false;
    }

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                 s_config.ssid.c_str(),
                 sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                 s_config.password.c_str(),
                 sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(err));
    }

    return true;
}

}  // namespace

bool begin(const Config& config)
{
    s_config = config;

    if (!ensureInitialized()) {
        return false;
    }

    if (!applyConfig()) {
        return false;
    }

    if (!s_started) {
        ESP_LOGI(TAG, "Starting Wi-Fi STA: %s", s_config.ssid.c_str());
        const esp_err_t err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            return false;
        }
    } else if (!s_connected) {
        recoverConnection();
    }

    if (s_connected) {
        return true;
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_WAIT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected");
        return true;
    }

    ESP_LOGW(TAG, "Wi-Fi not connected yet; background reconnect will continue");
    return false;
}

void recoverConnection()
{
    if (!s_initialized) {
        if (!s_config.ssid.empty()) {
            (void)begin(s_config);
        }
        return;
    }

    if (!s_started) {
        const esp_err_t err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_wifi_start failed during recovery: %s", esp_err_to_name(err));
            return;
        }
    }

    if (!s_connected) {
        ESP_LOGI(TAG, "Wi-Fi recovery requested");
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        }
    }
}

bool isStarted()
{
    return s_started;
}

bool isConnected()
{
    return s_connected;
}

const char* ssid()
{
    return s_config.ssid.empty() ? "" : s_config.ssid.c_str();
}

const char* statusText()
{
    if (!s_initialized || !s_started) {
        return "WiFi --";
    }
    return s_connected ? "WiFi OK" : "WiFi ...";
}

}  // namespace common::wifi
