#include "counter_mqtt.h"

#include "../../main/secrets.h"

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <mqtt_client.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace counter_mqtt {
namespace {

static constexpr const char* TAG = "CounterMQTT";
static constexpr const char* COUNTER_STATE_TOPIC = "counters/capacity/state";

esp_mqtt_client_handle_t s_client = nullptr;
bool s_started = false;
bool s_connected = false;
bool s_has_latest = false;
bool s_netif_ready = false;
int32_t s_latest_value = 0;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

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

void setLatestValue(int32_t value)
{
    portENTER_CRITICAL(&s_lock);
    s_latest_value = value;
    s_has_latest = true;
    portEXIT_CRITICAL(&s_lock);
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

    char payload[24] = {};
    const int copy_len = std::min(event->data_len, static_cast<int>(sizeof(payload) - 1));
    std::memcpy(payload, event->data, copy_len);
    payload[copy_len] = '\0';

    char* end = nullptr;
    long parsed = std::strtol(payload, &end, 10);
    if (end == payload) {
        ESP_LOGW(TAG, "Ignoring non-integer payload: %s", payload);
        return;
    }
    if (parsed < 0) {
        parsed = 0;
    }

    setLatestValue(static_cast<int32_t>(parsed));
    ESP_LOGI(TAG, "Received %s = %ld", COUNTER_STATE_TOPIC, parsed);
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

    if (!ensureNetworkStackReady()) {
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
    if (!s_started) {
        return "MQTT --";
    }
    return s_connected ? "MQTT OK" : "MQTT ...";
}

}  // namespace counter_mqtt
