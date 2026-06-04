#include "configure_mqtt_test.h"

#include <cstring>
#include <string>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mqtt_client.h>

namespace configure_mqtt_test {
namespace {

constexpr const char* TAG = "ConfigureMqttTest";
constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
constexpr EventBits_t WIFI_FAILED_BIT = BIT1;
constexpr EventBits_t MQTT_TEST_CONNECTED_BIT = BIT2;
constexpr EventBits_t MQTT_TEST_ERROR_BIT = BIT3;
constexpr int WIFI_MAX_RETRY = 8;

struct TestContext {
    EventGroupHandle_t event_group = nullptr;
    int wifi_retry_count = 0;
};

esp_netif_t* s_sta_netif = nullptr;
bool s_handlers_registered = false;
TestContext* s_active_ctx = nullptr;

void wifiEventHandler(void*, esp_event_base_t event_base, int32_t event_id, void*)
{
    auto* ctx = s_active_ctx;
    if (ctx == nullptr || ctx->event_group == nullptr) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (ctx->wifi_retry_count < WIFI_MAX_RETRY) {
            ++ctx->wifi_retry_count;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d/%d", ctx->wifi_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(ctx->event_group, WIFI_FAILED_BIT);
        }
    }
}

void ipEventHandler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    auto* ctx = s_active_ctx;
    if (ctx == nullptr || ctx->event_group == nullptr) {
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Wi-Fi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(ctx->event_group, WIFI_CONNECTED_BIT);
    }
}

void mqttTestEventHandler(void* handler_args, esp_event_base_t, int32_t event_id, void*)
{
    auto* ctx = static_cast<TestContext*>(handler_args);
    if (ctx == nullptr || ctx->event_group == nullptr) {
        return;
    }

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            xEventGroupSetBits(ctx->event_group, MQTT_TEST_CONNECTED_BIT);
            break;
        case MQTT_EVENT_ERROR:
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupSetBits(ctx->event_group, MQTT_TEST_ERROR_BIT);
            break;
        default:
            break;
    }
}

bool ensureStaReady()
{
    if (s_sta_netif == nullptr) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == nullptr) {
            ESP_LOGE(TAG, "Failed to create STA netif");
            return false;
        }
    }

    if (!s_handlers_registered) {
        esp_err_t err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifiEventHandler, nullptr);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to register Wi-Fi handler: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ipEventHandler, nullptr);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to register IP handler: %s", esp_err_to_name(err));
            return false;
        }
        s_handlers_registered = true;
    }

    return true;
}

std::string connectWifiForTest(TestContext& ctx,
                               const std::string& wifi_ssid,
                               const std::string& wifi_password)
{
    if (wifi_ssid.empty()) {
        return "MQTT test failed: Wi-Fi Network (SSID) is empty.";
    }
    if (!ensureStaReady()) {
        return "MQTT test failed: could not prepare Wi-Fi STA interface.";
    }

    wifi_config_t sta_config = {};
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.ssid),
                 wifi_ssid.c_str(),
                 sizeof(sta_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.password),
                 wifi_password.c_str(),
                 sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    s_active_ctx = &ctx;
    ctx.wifi_retry_count = 0;
    xEventGroupClearBits(ctx.event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        s_active_ctx = nullptr;
        return std::string("MQTT test failed: could not apply Wi-Fi config: ") + esp_err_to_name(err);
    }

    ESP_LOGI(TAG, "Connecting STA for MQTT test to SSID: %s", wifi_ssid.c_str());
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        s_active_ctx = nullptr;
        return std::string("MQTT test failed: could not start Wi-Fi connection: ") + esp_err_to_name(err);
    }

    EventBits_t bits = xEventGroupWaitBits(ctx.event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        return "";
    }

    esp_wifi_disconnect();
    s_active_ctx = nullptr;
    if (bits & WIFI_FAILED_BIT) {
        return "MQTT test failed: could not join Wi-Fi. Check SSID/password.";
    }
    return "MQTT test failed: timed out joining Wi-Fi.";
}

std::string runMqttTest(TestContext& ctx,
                        const std::string& uri,
                        const std::string& username,
                        const std::string& password)
{
    if (uri.empty()) {
        return "MQTT test failed: broker URI is empty.";
    }

    xEventGroupClearBits(ctx.event_group, MQTT_TEST_CONNECTED_BIT | MQTT_TEST_ERROR_BIT);

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = uri.c_str();
    if (!username.empty()) {
        mqtt_cfg.credentials.username = username.c_str();
    }
    if (!password.empty()) {
        mqtt_cfg.credentials.authentication.password = password.c_str();
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == nullptr) {
        return "MQTT test failed: could not create test client.";
    }

    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqttTestEventHandler, &ctx);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(client);
        return "MQTT test failed: could not start test client.";
    }

    EventBits_t bits = xEventGroupWaitBits(ctx.event_group,
                                           MQTT_TEST_CONNECTED_BIT | MQTT_TEST_ERROR_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(8000));

    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);

    if (bits & MQTT_TEST_CONNECTED_BIT) {
        return "MQTT connection successful.";
    }
    if (bits & MQTT_TEST_ERROR_BIT) {
        return "MQTT test failed: connection error, authentication failure, or broker refused connection.";
    }
    return "MQTT test failed: timeout. Confirm the broker address, port, and network path.";
}

}  // namespace

std::string testConnection(const std::string& wifi_ssid,
                           const std::string& wifi_password,
                           const std::string& uri,
                           const std::string& username,
                           const std::string& password)
{
    TestContext ctx;
    ctx.event_group = xEventGroupCreate();
    if (ctx.event_group == nullptr) {
        return "MQTT test failed: could not allocate test resources.";
    }

    std::string result = connectWifiForTest(ctx, wifi_ssid, wifi_password);
    if (result.empty()) {
        result = runMqttTest(ctx, uri, username, password);
    }

    esp_wifi_disconnect();
    if (s_active_ctx == &ctx) {
        s_active_ctx = nullptr;
    }
    vEventGroupDelete(ctx.event_group);

    return result;
}

}  // namespace configure_mqtt_test
