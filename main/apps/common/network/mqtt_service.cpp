#include "mqtt_service.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <esp_err.h>
#include <esp_log.h>
#include <mqtt_client.h>

namespace common::mqtt {
namespace {

static constexpr const char* TAG = "MqttService";
static constexpr size_t MAX_SUBSCRIPTIONS = 16;
static constexpr size_t MAX_PAYLOAD_SIZE = 512;

struct Subscription {
    std::string topic;
    int qos = 1;
    MessageCallback callback = nullptr;
    void* user_data = nullptr;
};

Config s_config;
esp_mqtt_client_handle_t s_client = nullptr;
bool s_started = false;
bool s_connected = false;
bool s_recovery_paused = false;
std::vector<Subscription> s_subscriptions;

bool topicMatches(const char* event_topic, int event_topic_len, const std::string& topic)
{
    if (event_topic == nullptr || topic.empty()) {
        return false;
    }

    return event_topic_len == static_cast<int>(topic.size()) &&
           std::strncmp(event_topic, topic.c_str(), event_topic_len) == 0;
}

void subscribeAll()
{
    if (s_client == nullptr || !s_connected) {
        return;
    }

    for (const auto& subscription : s_subscriptions) {
        if (!subscription.topic.empty()) {
            esp_mqtt_client_subscribe(s_client, subscription.topic.c_str(), subscription.qos);
            ESP_LOGI(TAG, "Subscribed: %s", subscription.topic.c_str());
        }
    }
}

void dispatchData(esp_mqtt_event_handle_t event)
{
    if (event == nullptr || event->topic == nullptr || event->data == nullptr) {
        return;
    }

    char payload[MAX_PAYLOAD_SIZE] = {};
    const int copy_len = std::min(event->data_len, static_cast<int>(sizeof(payload) - 1));
    std::memcpy(payload, event->data, copy_len);
    payload[copy_len] = '\0';

    char topic[256] = {};
    const int topic_copy_len = std::min(event->topic_len, static_cast<int>(sizeof(topic) - 1));
    std::memcpy(topic, event->topic, topic_copy_len);
    topic[topic_copy_len] = '\0';

    for (const auto& subscription : s_subscriptions) {
        if (topicMatches(event->topic, event->topic_len, subscription.topic) && subscription.callback != nullptr) {
            subscription.callback(topic, payload, subscription.user_data);
        }
    }
}

void eventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "Connected");
            subscribeAll();
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "Disconnected");
            break;

        case MQTT_EVENT_DATA:
            dispatchData(event);
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

bool begin(const Config& config)
{
    s_config = config;

    if (s_started) {
        recoverConnection();
        return true;
    }

    if (s_config.uri.empty()) {
        ESP_LOGE(TAG, "MQTT broker URI is empty");
        return false;
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = s_config.uri.c_str();
    if (!s_config.client_id.empty()) {
        mqtt_cfg.credentials.client_id = s_config.client_id.c_str();
    }
    if (!s_config.username.empty()) {
        mqtt_cfg.credentials.username = s_config.username.c_str();
    }
    if (!s_config.password.empty()) {
        mqtt_cfg.credentials.authentication.password = s_config.password.c_str();
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return false;
    }

    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, eventHandler, nullptr);

    const esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return false;
    }

    s_started = true;
    ESP_LOGI(TAG, "Started: %s", s_config.uri.c_str());
    return true;
}

void recoverConnection()
{
    if (s_recovery_paused) {
        ESP_LOGI(TAG, "MQTT recovery skipped; paused");
        return;
    }

    if (s_client == nullptr) {
        if (!s_config.uri.empty()) {
            (void)begin(s_config);
        }
        return;
    }

    if (!s_started) {
        return;
    }

    if (!s_connected) {
        ESP_LOGI(TAG, "MQTT recovery requested");
        const esp_err_t err = esp_mqtt_client_reconnect(s_client);
        if (err == ESP_ERR_INVALID_STATE || err == ESP_FAIL) {
            // The esp-mqtt task may still be starting immediately after
            // esp_mqtt_client_start(). In that state it will connect on its own;
            // forcing reconnect only creates a harmless but noisy boot warning.
            ESP_LOGD(TAG, "MQTT reconnect skipped while client is starting: %s", esp_err_to_name(err));
            return;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_mqtt_client_reconnect failed: %s", esp_err_to_name(err));
        }
    }
}


void setRecoveryPaused(bool paused)
{
    if (s_recovery_paused == paused) {
        return;
    }

    s_recovery_paused = paused;

    if (paused) {
        ESP_LOGI(TAG, "MQTT recovery paused");

        if (s_client != nullptr) {
            esp_mqtt_client_disconnect(s_client);
            esp_mqtt_client_stop(s_client);
        }

        s_started = false;
        s_connected = false;
    } else {
        ESP_LOGI(TAG, "MQTT recovery resumed");
    }
}

bool isRecoveryPaused()
{
    return s_recovery_paused;
}


bool isStarted()
{
    return s_started;
}

bool isConnected()
{
    return s_connected;
}

bool subscribe(const char* topic, int qos, MessageCallback callback, void* user_data)
{
    if (topic == nullptr || topic[0] == '\0') {
        ESP_LOGW(TAG, "Subscribe skipped, topic is empty");
        return false;
    }

    for (auto& subscription : s_subscriptions) {
        if (subscription.topic == topic) {
            subscription.qos = qos;
            subscription.callback = callback;
            subscription.user_data = user_data;
            if (s_client != nullptr && s_connected) {
                esp_mqtt_client_subscribe(s_client, subscription.topic.c_str(), subscription.qos);
            }
            return true;
        }
    }

    if (s_subscriptions.size() >= MAX_SUBSCRIPTIONS) {
        ESP_LOGW(TAG, "Subscribe skipped, subscription table full");
        return false;
    }

    s_subscriptions.push_back(Subscription{topic, qos, callback, user_data});

    if (s_client != nullptr && s_connected) {
        esp_mqtt_client_subscribe(s_client, topic, qos);
        ESP_LOGI(TAG, "Subscribed: %s", topic);
    }

    return true;
}

bool publish(const char* topic, const char* payload, int qos, bool retain)
{
    if (s_recovery_paused) {
        ESP_LOGI(TAG, "Publish skipped, MQTT recovery paused");
        return false;
    }

    if (!s_started || !s_connected || s_client == nullptr) {
        ESP_LOGW(TAG, "Publish skipped, MQTT not connected");
        return false;
    }

    if (topic == nullptr || topic[0] == '\0') {
        ESP_LOGW(TAG, "Publish skipped, topic is empty");
        return false;
    }

    if (payload == nullptr) {
        payload = "";
    }

    const int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Publish failed: %s", topic);
        return false;
    }

    ESP_LOGI(TAG, "Published: %s", topic);
    return true;
}

const char* brokerUri()
{
    return s_config.uri.empty() ? "" : s_config.uri.c_str();
}

const char* statusText()
{
    if (s_recovery_paused) {
        return "MQTT pause";
    }
    if (!s_started) {
        return "MQTT --";
    }
    return s_connected ? "MQTT OK" : "MQTT ...";
}

}  // namespace common::mqtt
