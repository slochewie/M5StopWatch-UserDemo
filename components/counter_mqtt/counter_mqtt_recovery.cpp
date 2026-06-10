#include "counter_mqtt.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_wifi.h>

namespace counter_mqtt {

void recoverConnection()
{
    static constexpr const char* TAG = "CounterMQTT";

    ESP_LOGI(TAG, "Wi-Fi recovery requested");

    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

}  // namespace counter_mqtt
