#include "configure_mqtt_test.h"

#include <mqtt_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

namespace configure_mqtt_test {
namespace {

constexpr EventBits_t MQTT_TEST_CONNECTED_BIT = BIT0;
constexpr EventBits_t MQTT_TEST_ERROR_BIT = BIT1;

struct MqttTestContext {
    EventGroupHandle_t event_group = nullptr;
};

void mqttTestEventHandler(void* handler_args, esp_event_base_t, int32_t event_id, void*)
{
    auto* ctx = static_cast<MqttTestContext*>(handler_args);
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

}  // namespace

std::string testConnection(const std::string& uri,
                           const std::string& username,
                           const std::string& password)
{
    if (uri.empty()) {
        return "MQTT test failed: broker URI is empty.";
    }

    MqttTestContext ctx;
    ctx.event_group = xEventGroupCreate();
    if (ctx.event_group == nullptr) {
        return "MQTT test failed: could not allocate test resources.";
    }

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
        vEventGroupDelete(ctx.event_group);
        return "MQTT test failed: could not create test client.";
    }

    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqttTestEventHandler, &ctx);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(client);
        vEventGroupDelete(ctx.event_group);
        return "MQTT test failed: could not start test client.";
    }

    EventBits_t bits = xEventGroupWaitBits(ctx.event_group,
                                           MQTT_TEST_CONNECTED_BIT | MQTT_TEST_ERROR_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(8000));

    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    vEventGroupDelete(ctx.event_group);

    if (bits & MQTT_TEST_CONNECTED_BIT) {
        return "MQTT connection successful.";
    }
    if (bits & MQTT_TEST_ERROR_BIT) {
        return "MQTT test failed: connection error, authentication failure, or broker refused connection.";
    }
    return "MQTT test failed: timeout. Confirm the broker address, port, and network path.";
}

}  // namespace configure_mqtt_test
