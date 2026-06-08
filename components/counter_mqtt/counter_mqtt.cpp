#include "counter_mqtt.h"

#include <device_config.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/portmacro.h>
#include <mqtt_client.h>
#include <nvs_flash.h>

namespace counter_mqtt {
namespace {

static constexpr const char* TAG = "CounterMQTT";
static constexpr const char* TIME_TOPIC = "system/time/epoch";
static constexpr time_t MIN_VALID_EPOCH = 1700000000;  // 2023-11-14 sanity floor.
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

device_config::Config s_config;
std::string s_counter_topic;
std::string s_battery_topic;

std::string deriveBatteryTopic(const std::string& counter_topic)
{
    static constexpr const char* suffix = "/state";
    static constexpr size_t suffix_len = 6;

    if (counter_topic.size() >= suffix_len &&
        counter_topic.compare(counter_topic.size() - suffix_len, suffix_len, suffix) == 0) {
        return counter_topic.substr(0, counter_topic.size() - suffix_len) + "/battery";
    }

    if (!counter_topic.empty() && counter_topic.back() == '/') {
        return counter_topic + "battery";
    }

    return counter_topic + "/battery";
}

void loadRuntimeConfig()
{
    s_config = device_config::load();

    auto defaults = device_config::defaults();
    if (s_config.device_name.empty()) {
        s_config.device_name = defaults.device_name;
    }
    if (s_config.mqtt_uri.empty()) {
        s_config.mqtt_uri = defaults.mqtt_uri;
    }
    if (s_config.counter_topic.empty()) {
        s_config.counter_topic = defaults.counter_topic;
    }

    s_counter_topic = s_config.counter_topic;
    s_battery_topic = deriveBatteryTopic(s_counter_topic);

    ESP_LOGI(TAG, "Loaded config: device=%s, broker=%s, topic=%s, battery=%s, ssid=%s",
             s_config.device_name.c_str(),
             s_config.mqtt_uri.c_str(),
             s_counter_topic.c_str(),
             s_battery_topic.c_str(),
             s_config.wifi_ssid.empty() ? "<empty>" : s_config.wifi_ssid.c_str());
}

void wifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_started = false;
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

    if (s_config.wifi_ssid.empty()) {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty. Open Configure and save Wi-Fi settings.");
        return false;
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
    cfg.nvs_enable = false;
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
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                 s_config.wifi_ssid.c_str(),
                 sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                 s_config.wifi_password.c_str(),
                 sizeof(wifi_config.sta.password) - 1);
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

    ESP_LOGI(TAG, "Starting Wi-Fi STA: %s", s_config.wifi_ssid.c_str());
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

bool parseEpochPayload(const char* payload, time_t& epoch)
{
    if (payload == nullptr) {
        return false;
    }

    char* end = nullptr;
    long long parsed = std::strtoll(payload, &end, 10);
    if (end != payload) {
        epoch = static_cast<time_t>(parsed);
        return epoch >= MIN_VALID_EPOCH;
    }

    const char* key = std::strstr(payload, "epoch");
    if (key == nullptr) {
        key = std::strstr(payload, "value");
    }
    if (key == nullptr) {
        return false;
    }

    const char* colon = std::strchr(key, ':');
    if (colon == nullptr) {
        return false;
    }

    parsed = std::strtoll(colon + 1, &end, 10);
    if (end == colon + 1) {
        return false;
    }

    epoch = static_cast<time_t>(parsed);
    return epoch >= MIN_VALID_EPOCH;
}

bool eventTopicMatches(esp_mqtt_event_handle_t event, const char* topic)
{
    if (event == nullptr || event->topic == nullptr || topic == nullptr || topic[0] == '\0') {
        return false;
    }

    const size_t topic_len = std::strlen(topic);
    return event->topic_len == static_cast<int>(topic_len) &&
           std::strncmp(event->topic, topic, event->topic_len) == 0;
}

void handleCounterData(const char* payload)
{
    int32_t parsed = 0;
    if (!parseCounterPayload(payload, parsed)) {
        ESP_LOGW(TAG, "Ignoring unsupported counter payload: %s", payload);
        return;
    }

    setLatestValue(parsed);
    ESP_LOGI(TAG, "Received %s = %ld", s_counter_topic.c_str(), static_cast<long>(parsed));
}

void handleTimeData(const char* payload)
{
    time_t epoch = 0;
    if (!parseEpochPayload(payload, epoch)) {
        ESP_LOGW(TAG, "Ignoring unsupported time payload: %s", payload);
        return;
    }

    timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0,
    };

    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGW(TAG, "settimeofday failed for epoch %lld", static_cast<long long>(epoch));
        return;
    }

    ESP_LOGI(TAG, "System time synced from %s: %lld", TIME_TOPIC, static_cast<long long>(epoch));
}

void handleData(esp_mqtt_event_handle_t event)
{
    if (event == nullptr || event->topic == nullptr || event->data == nullptr) {
        return;
    }

    char payload[128] = {};
    const int copy_len = std::min(event->data_len, static_cast<int>(sizeof(payload) - 1));
    std::memcpy(payload, event->data, copy_len);
    payload[copy_len] = '\0';

    if (eventTopicMatches(event, s_counter_topic.c_str())) {
        handleCounterData(payload);
        return;
    }

    if (eventTopicMatches(event, TIME_TOPIC)) {
        handleTimeData(payload);
        return;
    }
}

void mqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "Connected");
            if (!s_counter_topic.empty()) {
                esp_mqtt_client_subscribe(s_client, s_counter_topic.c_str(), 1);
                ESP_LOGI(TAG, "Subscribed: %s", s_counter_topic.c_str());
            }
            esp_mqtt_client_subscribe(s_client, TIME_TOPIC, 1);
            ESP_LOGI(TAG, "Subscribed: %s", TIME_TOPIC);
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

    loadRuntimeConfig();

    if (!ensureWifiConnected()) {
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = s_config.mqtt_uri.c_str();
    if (!s_config.device_name.empty()) {
        mqtt_cfg.credentials.client_id = s_config.device_name.c_str();
    }
    if (!s_config.mqtt_username.empty()) {
        mqtt_cfg.credentials.username = s_config.mqtt_username.c_str();
    }
    if (!s_config.mqtt_password.empty()) {
        mqtt_cfg.credentials.authentication.password = s_config.mqtt_password.c_str();
    }

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

    if (s_counter_topic.empty()) {
        ESP_LOGW(TAG, "Publish skipped, counter topic is empty");
        return false;
    }

    if (value < 0) {
        value = 0;
    }

    char payload[128];
    std::snprintf(payload,
                  sizeof(payload),
                  "{\"value\":%ld,\"updated_by\":\"%s\"}",
                  static_cast<long>(value),
                  s_config.device_name.empty() ? "m5stopwatch" : s_config.device_name.c_str());

    int msg_id = esp_mqtt_client_publish(s_client, s_counter_topic.c_str(), payload, 0, 1, 1);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Publish failed: %ld", static_cast<long>(value));
        return false;
    }

    ESP_LOGI(TAG, "Published %s = %ld", s_counter_topic.c_str(), static_cast<long>(value));
    return true;
}

bool publishBatteryPercentage(uint8_t percent)
{
    if (!s_started || !s_connected || s_client == nullptr) {
        ESP_LOGW(TAG, "Battery publish skipped, MQTT not connected");
        return false;
    }

    if (s_battery_topic.empty()) {
        ESP_LOGW(TAG, "Battery publish skipped, topic is empty");
        return false;
    }

    if (percent > 100) {
        percent = 100;
    }

    char payload[128];
    std::snprintf(payload,
                  sizeof(payload),
                  "{\"battery\":%u,\"device\":\"%s\"}",
                  static_cast<unsigned>(percent),
                  s_config.device_name.empty() ? "m5stopwatch" : s_config.device_name.c_str());

    int msg_id = esp_mqtt_client_publish(s_client, s_battery_topic.c_str(), payload, 0, 1, 1);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Battery publish failed: %u", static_cast<unsigned>(percent));
        return false;
    }

    ESP_LOGI(TAG, "Published %s = %u", s_battery_topic.c_str(), static_cast<unsigned>(percent));
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

const char* brokerUri()
{
    return s_config.mqtt_uri.empty() ? "" : s_config.mqtt_uri.c_str();
}

const char* counterTopic()
{
    return s_counter_topic.empty() ? "" : s_counter_topic.c_str();
}

const char* batteryTopic()
{
    return s_battery_topic.empty() ? "" : s_battery_topic.c_str();
}

const char* deviceName()
{
    return s_config.device_name.empty() ? "" : s_config.device_name.c_str();
}

const char* wifiSsid()
{
    return s_config.wifi_ssid.empty() ? "" : s_config.wifi_ssid.c_str();
}

}  // namespace counter_mqtt
