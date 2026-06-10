#include "counter_mqtt.h"

#include <device_config.h>
#include <hal/hal.h>
#include <apps/common/network/mqtt_service.h>
#include <apps/common/network/wifi_service.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace counter_mqtt {
namespace {

static constexpr const char* TAG = "CounterMQTT";
static constexpr const char* TIME_TOPIC = "system/time/epoch";
static constexpr const char* LOCAL_TIMEZONE = "PST8PDT,M3.2.0,M11.1.0";
static constexpr time_t MIN_VALID_EPOCH = 1700000000;  // 2023-11-14 sanity floor.

device_config::Config s_config;
std::string s_counter_topic;
std::string s_battery_topic;
bool s_loaded = false;
bool s_started = false;
bool s_has_latest = false;
int32_t s_latest_value = 0;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void applyLocalTimezone()
{
    // Store this in HAL settings too. HAL::rtc_init() otherwise defaults to GMT0,
    // which makes App Setup and the launcher header show UTC.
    GetHAL().setTimezone(LOCAL_TIMEZONE);
}

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
    applyLocalTimezone();
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
    s_loaded = true;

    ESP_LOGI(TAG, "Loaded config: device=%s, broker=%s, topic=%s, battery=%s, ssid=%s",
             s_config.device_name.c_str(),
             s_config.mqtt_uri.c_str(),
             s_counter_topic.c_str(),
             s_battery_topic.c_str(),
             s_config.wifi_ssid.empty() ? "<empty>" : s_config.wifi_ssid.c_str());
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

void handleCounterData(const char* payload)
{
    int32_t parsed = 0;
    if (!parseCounterPayload(payload, parsed)) {
        ESP_LOGW(TAG, "Ignoring unsupported counter payload: %s", payload == nullptr ? "<null>" : payload);
        return;
    }

    setLatestValue(parsed);
    ESP_LOGI(TAG, "Received %s = %ld", s_counter_topic.c_str(), static_cast<long>(parsed));
}

void handleTimeData(const char* payload)
{
    time_t epoch = 0;
    if (!parseEpochPayload(payload, epoch)) {
        ESP_LOGW(TAG, "Ignoring unsupported time payload: %s", payload == nullptr ? "<null>" : payload);
        return;
    }

    applyLocalTimezone();

    timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0,
    };

    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGW(TAG, "settimeofday failed for epoch %lld", static_cast<long long>(epoch));
        return;
    }

    // Keep the hardware RTC in sync with the corrected system epoch. HAL stores
    // the timezone in NVS, so Launcher and App Setup use Pacific local time.
    GetHAL().syncSystemTimeToRtc();

    std::tm local_tm = {};
    if (localtime_r(&epoch, &local_tm) != nullptr) {
        ESP_LOGI(TAG,
                 "System time synced from %s: epoch=%lld local=%04d-%02d-%02d %02d:%02d:%02d TZ=%s",
                 TIME_TOPIC,
                 static_cast<long long>(epoch),
                 local_tm.tm_year + 1900,
                 local_tm.tm_mon + 1,
                 local_tm.tm_mday,
                 local_tm.tm_hour,
                 local_tm.tm_min,
                 local_tm.tm_sec,
                 getenv("TZ") == nullptr ? "<unset>" : getenv("TZ"));
    } else {
        ESP_LOGI(TAG,
                 "System time synced from %s: epoch=%lld TZ=%s",
                 TIME_TOPIC,
                 static_cast<long long>(epoch),
                 getenv("TZ") == nullptr ? "<unset>" : getenv("TZ"));
    }
}

void handleMqttMessage(const char* topic, const char* payload, void* user_data)
{
    if (topic == nullptr) {
        return;
    }

    if (s_counter_topic == topic) {
        handleCounterData(payload);
        return;
    }

    if (std::strcmp(topic, TIME_TOPIC) == 0) {
        handleTimeData(payload);
        return;
    }
}

}  // namespace

void begin()
{
    if (s_started) {
        common::wifi::recoverConnection();
        common::mqtt::recoverConnection();
        return;
    }

    loadRuntimeConfig();

    common::wifi::Config wifi_config = {
        .ssid = s_config.wifi_ssid,
        .password = s_config.wifi_password,
    };

    if (!common::wifi::begin(wifi_config)) {
        ESP_LOGW(TAG, "Wi-Fi not connected yet; MQTT start deferred");
        return;
    }

    common::mqtt::subscribe(s_counter_topic.c_str(), 1, handleMqttMessage);
    common::mqtt::subscribe(TIME_TOPIC, 1, handleMqttMessage);

    common::mqtt::Config mqtt_config = {
        .uri = s_config.mqtt_uri,
        .client_id = s_config.device_name,
        .username = s_config.mqtt_username,
        .password = s_config.mqtt_password,
    };

    if (!common::mqtt::begin(mqtt_config)) {
        ESP_LOGW(TAG, "MQTT not started yet");
        return;
    }

    s_started = true;
    ESP_LOGI(TAG, "Started");
}

bool isStarted()
{
    return s_started || common::mqtt::isStarted();
}

bool isConnected()
{
    return common::mqtt::isConnected();
}

bool publishCounterValue(int32_t value)
{
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

    const bool ok = common::mqtt::publish(s_counter_topic.c_str(), payload, 1, true);
    if (ok) {
        ESP_LOGI(TAG, "Published %s = %ld", s_counter_topic.c_str(), static_cast<long>(value));
    }
    return ok;
}

bool publishBatteryPercentage(uint8_t percent)
{
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

    const bool ok = common::mqtt::publish(s_battery_topic.c_str(), payload, 1, true);
    if (ok) {
        ESP_LOGI(TAG, "Published %s = %u", s_battery_topic.c_str(), static_cast<unsigned>(percent));
    }
    return ok;
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
    if (!common::wifi::isConnected()) {
        return common::wifi::statusText();
    }
    return common::mqtt::statusText();
}

const char* brokerUri()
{
    return s_config.mqtt_uri.empty() ? common::mqtt::brokerUri() : s_config.mqtt_uri.c_str();
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
    return s_config.wifi_ssid.empty() ? common::wifi::ssid() : s_config.wifi_ssid.c_str();
}

}  // namespace counter_mqtt
