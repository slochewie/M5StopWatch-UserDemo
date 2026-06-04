#include "counter_mqtt.h"

#include "../../main/secrets.h"

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <mqtt_client.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/portmacro.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace counter_mqtt {
namespace {

static constexpr const char* TAG = "CounterMQTT";
static constexpr const char* COUNTER_STATE_TOPIC = "counters/capacity/state";
static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr int WIFI_FAIL_BIT = BIT1;
static constexpr int WIFI_MAXIMUM_RETRY = 8;

esp_mqtt_client_handle_t s_client = nullptr;
EventGroupHandle_t s_wifi_event_group = nullptr;
bool s_started = false;
bool s_connected = false;
bool s_has_latest = false;
bool s_netif_ready = false;
bool s_wifi_started = false;
int s_wifi_retry_count = 0;
int32_t s_latest_value = 0;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void wifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < WIFI_MAXIMUM_RETRY) {
            ++s_wifi_retry_count;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d/%d", s_wifi_retry_count, WIFI_MAXIMUM_RETRY);
            esp_wifi_connect();
        } else if (s_wifi_event_group != nullptr) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        s_wifi_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_event_group != nullptr) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

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

bool ensureWifiConnected()
{
    if (s_wifi_started) {
        return true;
    }

    if (!ensureNetworkStackReady()) {
        return false;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == nullptr) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return false;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_event_handler_instance_t wifi_any_id = nullptr;
    esp_event_handler_instance_t got_ip = nullptr;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr, &wifi_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr, &got_ip);

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
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

    ESP_LOGI(TAG, "Starting Wi-Fi STA");
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        s_wifi_started = true;
        ESP_LOGI(TAG, "Wi-Fi connected");
        return true;
    }

    ESP_LOGW(TAG, "Wi-Fi not connected yet");
    return false;
}

void setLatestValue(int32_t value)
{
    portENTER_CRITICAL(&s_lock);
    s_latest_value = value;
    s_has_latest = true;
    portEXIT_CRITICAL(&s_lock);
}

bool parseCounterPayload(const char* payload, int32_t& value)
{
    if (payload == nullptr) {
        return false;
    }

    char* end = nullptr;
    long parsed = std::strtol(payload, &end, 10);
    if (end != payload) {
        value = static_cast<int32_t>(std::max<long>(parsed, 0));
        return true;
    }

    const char* key = std::strstr(payload, "value");
    if (key == nullptr) {
        return false;
    }

    const char* colon = std::strchr(key, ':');
    if (colon == nullptr) {
        return false;
    }

    parsed = std::strtol(colon + 1, &end, 10);
    if (end == colon + 1) {
        return false;
    }

    value = static_cast<int32_t>(std::max<long>(parsed, 0));
    return true;
}

void handleData(esp_mqtt_event_handle_t event)
{
    if (event == nullptr || event->topic == nullptr || event->data == nullptr) {
        return;
    }

    if (event->topic_len != static_cast<int>(std::strlen(COUNTER_STATE_TOPIC)) ||
        std::strncmp(event->topic, COUNTER_STATE_TOPIC, event->topic_len) != 0) {
        return;
    }

    char payload[128] = {};
    const int copy_len = std::min(event->data_len, static_cast<int>(sizeof(payload) - 1));
    std::memcpy(payload, event->data, copy_len);
    payload[copy_len] = '\0';

    int32_t parsed = 0;
    if (!parseCounterPayload(payload, parsed)) {
        ESP_LOGW(TAG, "Ignoring unsupported payload: %s", payload);
        return;
    }

    setLatestValue(parsed);
    ESP_LOGI(TAG, "Received %s = %ld", COUNTER_STATE_TOPIC, static_cast<long>(parsed));
}

void mqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "Connected");
            esp_mqtt_client_subscribe(s_client, COUNTER_STATE_TOPIC, 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "Disconnected");
            break;

        case MQTT_EVENT_DATA:
            handleData(event);
            break;

        case MQTT_EVENT_ERROR:
            s_connected = false;
            ESP_LOGW(TAG, "MQTT error");
            break;

        default:
            break;
    }
}

}  // namespace

void begin()
{
    if (s_started) {
        return;
    }

    if (!ensureWifiConnected()) {
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_BROKER_URI;
    mqtt_cfg.credentials.username = MQTT_USERNAME;
    mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqttEventHandler, nullptr);

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return;
    }

    s_started = true;
    ESP_LOGI(TAG, "Started");
}

bool isStarted()
{
    return s_started;
}

bool isConnected()
{
    return s_connected;
}

bool publishCounterValue(int32_t value)
{
    if (!s_started || !s_connected || s_client == nullptr) {
        ESP_LOGW(TAG, "Publish skipped, MQTT not connected");
        return false;
    }

    if (value < 0) {
        value = 0;
    }

    char payload[96];
    std::snprintf(payload, sizeof(payload), "{\"value\":%ld,\"updated_by\":\"m5stopwatch\"}", static_cast<long>(value));

    int msg_id = esp_mqtt_client_publish(s_client, COUNTER_STATE_TOPIC, payload, 0, 1, 1);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Publish failed: %ld", static_cast<long>(value));
        return false;
    }

    ESP_LOGI(TAG, "Published %s = %ld", COUNTER_STATE_TOPIC, static_cast<long>(value));
    return true;
}

bool takeLatestValue(int32_t& value)
{
    bool has_value = false;

    portENTER_CRITICAL(&s_lock);
    if (s_has_latest) {
        value = s_latest_value;
        s_has_latest = false;
        has_value = true;
    }
    portEXIT_CRITICAL(&s_lock);

    return has_value;
}

const char* statusText()
{
    if (!s_wifi_started) {
        return "WiFi ...";
    }
    if (!s_started) {
        return "MQTT --";
    }
    return s_connected ? "MQTT OK" : "MQTT ...";
}

}  // namespace counter_mqtt
