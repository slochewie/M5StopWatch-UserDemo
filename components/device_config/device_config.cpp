#include "device_config.h"

#include <esp_err.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cstring>

namespace device_config {
namespace {

constexpr const char* TAG = "DeviceConfig";
constexpr const char* NVS_NS = "counter_cfg";

bool ensureNvsReady()
{
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
    return true;
}

std::string readString(nvs_handle_t handle, const char* key, const std::string& fallback)
{
    size_t length = 0;
    esp_err_t err = nvs_get_str(handle, key, nullptr, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND || length == 0) {
        return fallback;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read %s: %s", key, esp_err_to_name(err));
        return fallback;
    }

    std::string value(length, '\0');
    err = nvs_get_str(handle, key, value.data(), &length);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read %s value: %s", key, esp_err_to_name(err));
        return fallback;
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value.empty() ? fallback : value;
}

bool writeString(nvs_handle_t handle, const char* key, const std::string& value)
{
    esp_err_t err = nvs_set_str(handle, key, value.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write %s: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

}  // namespace

Config defaults()
{
    return Config{};
}

Config load()
{
    Config config = defaults();
    if (!ensureNvsReady()) {
        return config;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return config;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return config;
    }

    config.device_name = readString(handle, "device", config.device_name);
    config.wifi_ssid = readString(handle, "wifi_ssid", config.wifi_ssid);
    config.wifi_password = readString(handle, "wifi_pass", config.wifi_password);
    config.mqtt_uri = readString(handle, "mqtt_uri", config.mqtt_uri);
    config.mqtt_username = readString(handle, "mqtt_user", config.mqtt_username);
    config.mqtt_password = readString(handle, "mqtt_pass", config.mqtt_password);
    config.counter_topic = readString(handle, "topic", config.counter_topic);

    nvs_close(handle);
    return config;
}

bool save(const Config& config)
{
    if (!ensureNvsReady()) {
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open write failed: %s", esp_err_to_name(err));
        return false;
    }

    bool ok = true;
    ok = writeString(handle, "device", config.device_name) && ok;
    ok = writeString(handle, "wifi_ssid", config.wifi_ssid) && ok;
    ok = writeString(handle, "wifi_pass", config.wifi_password) && ok;
    ok = writeString(handle, "mqtt_uri", config.mqtt_uri) && ok;
    ok = writeString(handle, "mqtt_user", config.mqtt_username) && ok;
    ok = writeString(handle, "mqtt_pass", config.mqtt_password) && ok;
    ok = writeString(handle, "topic", config.counter_topic) && ok;

    if (ok) {
        err = nvs_commit(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
            ok = false;
        }
    }

    nvs_close(handle);
    return ok;
}

void reset()
{
    if (!ensureNvsReady()) {
        return;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open reset failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace device_config
