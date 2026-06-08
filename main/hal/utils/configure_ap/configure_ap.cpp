/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "configure_ap.h"

#include <device_config.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <dns_server.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <lwip/ip_addr.h>
#include <mqtt_client.h>
#include <nvs_flash.h>

namespace configure_ap {
namespace {

constexpr const char* TAG = "ConfigureAP";
constexpr const char* AP_PREFIX = "M5Configure";
constexpr const char* AP_URL = "http://192.168.4.1";
constexpr EventBits_t EXIT_BIT = BIT0;
constexpr EventBits_t MQTT_TEST_CONNECTED_BIT = BIT0;
constexpr EventBits_t MQTT_TEST_ERROR_BIT = BIT1;
constexpr const char* JSON_TYPE = "application/json";
extern const char configure_ap_html_start[] asm("_binary_configure_ap_html_start");
extern const char configure_ap_html_end[] asm("_binary_configure_ap_html_end");

std::mutex g_session_mutex;
EventGroupHandle_t g_active_event_group = nullptr;

constexpr const char* CAPTIVE_URLS[] = {
    "/hotspot-detect.html", "/generate_204*", "/mobile/status.php", "/check_network_status.txt",
    "/ncsi.txt", "/fwlink/", "/connectivity-check.html", "/success.txt", "/portal.html",
    "/library/test/success.html",
};


struct MqttTestContext {
    EventGroupHandle_t event_group = nullptr;
};

void mqtt_test_event_handler(void* handler_args, esp_event_base_t, int32_t event_id, void*)
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

bool ensure_wifi_stack_ready()
{
    static std::mutex mutex;
    static bool initialized = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (initialized) {
        return true;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(ret));
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(ret));
        return false;
    }

    initialized = true;
    return true;
}

std::string make_ap_ssid()
{
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        return AP_PREFIX;
    }
    char ssid[32] = {};
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", AP_PREFIX, mac[4], mac[5]);
    return std::string(ssid);
}

std::string json_escape(std::string_view input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string config_to_json(const device_config::Config& c, std::string_view ap_ssid)
{
    std::string j = "{";
    j += "\"ap_ssid\":\"" + json_escape(ap_ssid) + "\",";
    j += "\"ap_url\":\"" + std::string(AP_URL) + "\",";
    j += "\"device_name\":\"" + json_escape(c.device_name) + "\",";
    j += "\"wifi_ssid\":\"" + json_escape(c.wifi_ssid) + "\",";
    j += "\"wifi_password\":\"" + json_escape(c.wifi_password) + "\",";
    j += "\"mqtt_uri\":\"" + json_escape(c.mqtt_uri) + "\",";
    j += "\"mqtt_username\":\"" + json_escape(c.mqtt_username) + "\",";
    j += "\"mqtt_password\":\"" + json_escape(c.mqtt_password) + "\",";
    j += "\"counter_topic\":\"" + json_escape(c.counter_topic) + "\"";
    j += "}";
    return j;
}

std::string get_json_string(std::string_view body, std::string_view key)
{
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return "";
    }
    pos = body.find(':', pos);
    if (pos == std::string_view::npos) {
        return "";
    }
    pos = body.find('"', pos);
    if (pos == std::string_view::npos) {
        return "";
    }
    ++pos;
    std::string out;
    bool esc = false;
    for (; pos < body.size(); ++pos) {
        char c = body[pos];
        if (esc) {
            out += c;
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

class Session {
public:
    explicit Session(const std::function<void(std::string_view)>& onLog) : _on_log(onLog) {}

    bool run()
    {
        if (!ensure_wifi_stack_ready()) {
            log("Wi-Fi initialization failed");
            return false;
        }
        _event_group = xEventGroupCreate();
        if (_event_group == nullptr) {
            log("Failed to create event group");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            g_active_event_group = _event_group;
        }
        if (!start_ap() || !start_server()) {
            stop();
            return false;
        }
        log("Connect to Wi-Fi: " + _ssid + "\nOpen: " + std::string(AP_URL));
        xEventGroupWaitBits(_event_group, EXIT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        stop();
        log("Configure portal closed");
        return true;
    }

private:
    void log(const std::string& msg) const
    {
        ESP_LOGI(TAG, "%s", msg.c_str());
        if (_on_log) {
            _on_log(msg);
        }
    }

    bool start_ap()
    {
        static esp_netif_t* ap_netif = nullptr;
        if (ap_netif == nullptr) {
            ap_netif = esp_netif_create_default_wifi_ap();
        }
        if (ap_netif == nullptr) {
            log("Failed to create AP interface");
            return false;
        }

        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_set_ip_info(ap_netif, &ip_info);
        esp_netif_dhcps_start(ap_netif);

        _dns_server = std::make_unique<DnsServer>();
        _dns_server->Start(ip_info.gw);
        _ssid = make_ap_ssid();

        wifi_config_t wifi_config = {};
        strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), _ssid.c_str(), sizeof(wifi_config.ap.ssid) - 1);
        wifi_config.ap.ssid_len = _ssid.size();
        wifi_config.ap.max_connection = 4;
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;

        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED && ret != ESP_ERR_WIFI_MODE) {
            ESP_LOGW(TAG, "wifi stop before AP failed: %s", esp_err_to_name(ret));
        }
        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) {
            log("Failed to set APSTA mode");
            return false;
        }
        ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
        if (ret != ESP_OK) {
            log("Failed to apply AP config");
            return false;
        }
        ret = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "disable power save failed: %s", esp_err_to_name(ret));
        }
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            log("Failed to start AP");
            return false;
        }
        return true;
    }

    bool start_server()
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers = 18;
        config.uri_match_fn = httpd_uri_match_wildcard;
        config.recv_wait_timeout = 15;
        config.send_wait_timeout = 15;
        esp_err_t ret = httpd_start(&_server, &config);
        if (ret != ESP_OK) {
            log("Failed to start web server");
            return false;
        }
        httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = &Session::handle_index, .user_ctx = this};
        httpd_uri_t config_get = {.uri = "/config", .method = HTTP_GET, .handler = &Session::handle_config_get, .user_ctx = this};
        httpd_uri_t config_post = {.uri = "/config", .method = HTTP_POST, .handler = &Session::handle_config_post, .user_ctx = this};
        httpd_uri_t mqtt_test = {.uri = "/mqtt/test", .method = HTTP_POST, .handler = &Session::handle_mqtt_test, .user_ctx = this};
        httpd_uri_t scan = {.uri = "/wifi/scan", .method = HTTP_GET, .handler = &Session::handle_wifi_scan, .user_ctx = this};
        httpd_uri_t close = {.uri = "/close", .method = HTTP_POST, .handler = &Session::handle_close, .user_ctx = this};
        httpd_uri_t reboot = {.uri = "/reboot", .method = HTTP_POST, .handler = &Session::handle_reboot, .user_ctx = this};
        httpd_uri_t captive = {.uri = nullptr, .method = HTTP_GET, .handler = &Session::handle_captive, .user_ctx = this};
        ret = httpd_register_uri_handler(_server, &index);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &config_get);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &config_post);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &mqtt_test);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &scan);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &close);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &reboot);
        if (ret == ESP_OK) {
            for (const auto* url : CAPTIVE_URLS) {
                captive.uri = url;
                ret = httpd_register_uri_handler(_server, &captive);
                if (ret != ESP_OK) break;
            }
        }
        if (ret != ESP_OK) {
            log("Failed to register routes");
            return false;
        }
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            if (g_active_event_group == _event_group) {
                g_active_event_group = nullptr;
            }
        }
        if (_server) {
            httpd_stop(_server);
            _server = nullptr;
        }
        if (_dns_server) {
            _dns_server->Stop();
            _dns_server.reset();
        }
        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED && ret != ESP_ERR_WIFI_MODE) {
            ESP_LOGW(TAG, "wifi stop failed: %s", esp_err_to_name(ret));
        }
        if (_event_group) {
            vEventGroupDelete(_event_group);
            _event_group = nullptr;
        }
    }

    static Session* self(httpd_req_t* req) { return static_cast<Session*>(req->user_ctx); }

    static void send_json(httpd_req_t* req, const std::string& body)
    {
        httpd_resp_set_type(req, JSON_TYPE);
        httpd_resp_send(req, body.c_str(), body.size());
    }

    static std::string read_body(httpd_req_t* req)
    {
        std::string body(static_cast<size_t>(req->content_len), '\0');
        size_t offset = 0;
        while (offset < body.size()) {
            int got = httpd_req_recv(req, body.data() + offset, body.size() - offset);
            if (got <= 0) {
                return "";
            }
            offset += static_cast<size_t>(got);
        }
        return body;
    }

    static esp_err_t handle_index(httpd_req_t* req)
    {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req,
                        configure_ap_html_start,
                        configure_ap_html_end - configure_ap_html_start);
        return ESP_OK;
    }


    static esp_err_t handle_config_get(httpd_req_t* req)
    {
        auto* s = self(req);
        send_json(req, config_to_json(device_config::load(), s ? s->_ssid : ""));
        return ESP_OK;
    }

    static esp_err_t handle_config_post(httpd_req_t* req)
    {
        std::string body = read_body(req);
        if (body.empty() && req->content_len > 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
            return ESP_FAIL;
        }

        device_config::Config cfg;
        cfg.device_name = get_json_string(body, "device_name");
        cfg.wifi_ssid = get_json_string(body, "wifi_ssid");
        cfg.wifi_password = get_json_string(body, "wifi_password");
        cfg.mqtt_uri = get_json_string(body, "mqtt_uri");
        cfg.mqtt_username = get_json_string(body, "mqtt_username");
        cfg.mqtt_password = get_json_string(body, "mqtt_password");
        cfg.counter_topic = get_json_string(body, "counter_topic");
        auto defaults = device_config::defaults();
        if (cfg.device_name.empty()) cfg.device_name = defaults.device_name;
        if (cfg.mqtt_uri.empty()) cfg.mqtt_uri = defaults.mqtt_uri;
        if (cfg.counter_topic.empty()) cfg.counter_topic = defaults.counter_topic;

        if (!device_config::save(cfg)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
            return ESP_FAIL;
        }
        httpd_resp_sendstr(req, "Saved. Changes will take effect after reboot.");
        return ESP_OK;
    }

    static esp_err_t handle_mqtt_test(httpd_req_t* req)
    {
        std::string body = read_body(req);
        if (body.empty() && req->content_len > 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
            return ESP_FAIL;
        }

        std::string uri = get_json_string(body, "mqtt_uri");
        std::string username = get_json_string(body, "mqtt_username");
        std::string password = get_json_string(body, "mqtt_password");
        if (uri.empty()) {
            httpd_resp_sendstr(req, "MQTT test failed: broker URI is empty.");
            return ESP_OK;
        }

        MqttTestContext ctx;
        ctx.event_group = xEventGroupCreate();
        if (ctx.event_group == nullptr) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "test allocation failed");
            return ESP_FAIL;
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
            httpd_resp_sendstr(req, "MQTT test failed: could not create test client.");
            return ESP_OK;
        }

        esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_test_event_handler, &ctx);
        esp_err_t err = esp_mqtt_client_start(client);
        if (err != ESP_OK) {
            esp_mqtt_client_destroy(client);
            vEventGroupDelete(ctx.event_group);
            httpd_resp_sendstr(req, "MQTT test failed: could not start test client.");
            return ESP_OK;
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
            httpd_resp_sendstr(req, "MQTT connection successful.");
        } else if (bits & MQTT_TEST_ERROR_BIT) {
            httpd_resp_sendstr(req, "MQTT test failed: connection error, authentication failure, or broker refused connection.");
        } else {
            httpd_resp_sendstr(req, "MQTT test failed: timeout. Confirm the broker address, port, and network path.");
        }
        return ESP_OK;
    }

    static esp_err_t handle_wifi_scan(httpd_req_t* req)
    {
        uint16_t ap_count = 0;
        wifi_scan_config_t scan_config = {};
        esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
        if (ret != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
            return ESP_FAIL;
        }
        esp_wifi_scan_get_ap_num(&ap_count);
        std::vector<wifi_ap_record_t> records(ap_count);
        if (ap_count > 0) {
            esp_wifi_scan_get_ap_records(&ap_count, records.data());
        }
        std::string json = "{\"networks\":[";
        for (uint16_t i = 0; i < ap_count; ++i) {
            if (i) json += ",";
            json += "{\"ssid\":\"" + json_escape(reinterpret_cast<const char*>(records[i].ssid)) + "\",";
            json += "\"rssi\":" + std::to_string(records[i].rssi) + "}";
        }
        json += "]}";
        send_json(req, json);
        return ESP_OK;
    }

    static esp_err_t handle_close(httpd_req_t* req)
    {
        auto* s = self(req);
        if (s && s->_event_group) {
            xEventGroupSetBits(s->_event_group, EXIT_BIT);
        }
        httpd_resp_sendstr(req, "closing");
        return ESP_OK;
    }

    static esp_err_t handle_reboot(httpd_req_t* req)
    {
        httpd_resp_sendstr(req, "rebooting");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return ESP_OK;
    }

    static esp_err_t handle_captive(httpd_req_t* req)
    {
        std::string url = std::string(AP_URL) + "/?_=" + std::to_string(esp_timer_get_time());
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", url.c_str());
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    httpd_handle_t _server = nullptr;
    EventGroupHandle_t _event_group = nullptr;
    std::function<void(std::string_view)> _on_log;
    std::string _ssid;
    std::unique_ptr<DnsServer> _dns_server;
};

}  // namespace

bool run(const std::function<void(std::string_view)>& onLog)
{
    return Session(onLog).run();
}

void requestStop()
{
    std::lock_guard<std::mutex> lock(g_session_mutex);
    if (g_active_event_group != nullptr) {
        xEventGroupSetBits(g_active_event_group, EXIT_BIT);
    }
}

}  // namespace configure_ap
namespace {

constexpr const char* TAG = "ConfigureAP";
constexpr const char* AP_PREFIX = "M5Configure";
constexpr const char* AP_URL = "http://192.168.4.1";
constexpr EventBits_t EXIT_BIT = BIT0;
constexpr EventBits_t MQTT_TEST_CONNECTED_BIT = BIT0;
constexpr EventBits_t MQTT_TEST_ERROR_BIT = BIT1;
constexpr const char* JSON_TYPE = "application/json";
constexpr const char* CSS_TYPE = "text/css; charset=utf-8";
constexpr const char* JS_TYPE = "application/javascript; charset=utf-8";

extern const char configure_ap_html_start[] asm("_binary_configure_ap_html_start");
extern const char configure_ap_html_end[] asm("_binary_configure_ap_html_end");
extern const char configure_ap_css_start[] asm("_binary_configure_ap_css_start");
extern const char configure_ap_css_end[] asm("_binary_configure_ap_css_end");
extern const char configure_ap_js_start[] asm("_binary_configure_ap_js_start");
extern const char configure_ap_js_end[] asm("_binary_configure_ap_js_end");

std::mutex g_session_mutex;
EventGroupHandle_t g_active_event_group = nullptr;

constexpr const char* CAPTIVE_URLS[] = {
    "/hotspot-detect.html", "/generate_204*", "/mobile/status.php", "/check_network_status.txt",
    "/ncsi.txt", "/fwlink/", "/connectivity-check.html", "/success.txt", "/portal.html",
    "/library/test/success.html",
};


struct MqttTestContext {
    EventGroupHandle_t event_group = nullptr;
};

void mqtt_test_event_handler(void* handler_args, esp_event_base_t, int32_t event_id, void*)
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

bool ensure_wifi_stack_ready()
{
    static std::mutex mutex;
    static bool initialized = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (initialized) {
        return true;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(ret));
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(ret));
        return false;
    }

    initialized = true;
    return true;
}

std::string make_ap_ssid()
{
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        return AP_PREFIX;
    }
    char ssid[32] = {};
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", AP_PREFIX, mac[4], mac[5]);
    return std::string(ssid);
}

std::string json_escape(std::string_view input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string config_to_json(const device_config::Config& c, std::string_view ap_ssid)
{
    std::string j = "{";
    j += "\"ap_ssid\":\"" + json_escape(ap_ssid) + "\",";
    j += "\"ap_url\":\"" + std::string(AP_URL) + "\",";
    j += "\"device_name\":\"" + json_escape(c.device_name) + "\",";
    j += "\"wifi_ssid\":\"" + json_escape(c.wifi_ssid) + "\",";
    j += "\"wifi_password\":\"" + json_escape(c.wifi_password) + "\",";
    j += "\"mqtt_uri\":\"" + json_escape(c.mqtt_uri) + "\",";
    j += "\"mqtt_username\":\"" + json_escape(c.mqtt_username) + "\",";
    j += "\"mqtt_password\":\"" + json_escape(c.mqtt_password) + "\",";
    j += "\"counter_topic\":\"" + json_escape(c.counter_topic) + "\"";
    j += "}";
    return j;
}

std::string get_json_string(std::string_view body, std::string_view key)
{
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return "";
    }
    pos = body.find(':', pos);
    if (pos == std::string_view::npos) {
        return "";
    }
    pos = body.find('"', pos);
    if (pos == std::string_view::npos) {
        return "";
    }
    ++pos;
    std::string out;
    bool esc = false;
    for (; pos < body.size(); ++pos) {
        char c = body[pos];
        if (esc) {
            out += c;
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

class Session {
public:
    explicit Session(const std::function<void(std::string_view)>& onLog) : _on_log(onLog) {}

    bool run()
    {
        if (!ensure_wifi_stack_ready()) {
            log("Wi-Fi initialization failed");
            return false;
        }
        _event_group = xEventGroupCreate();
        if (_event_group == nullptr) {
            log("Failed to create event group");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            g_active_event_group = _event_group;
        }
        if (!start_ap() || !start_server()) {
            stop();
            return false;
        }
        log("Connect to Wi-Fi: " + _ssid + "\nOpen: " + std::string(AP_URL));
        xEventGroupWaitBits(_event_group, EXIT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        stop();
        log("Configure portal closed");
        return true;
    }

private:
    void log(const std::string& msg) const
    {
        ESP_LOGI(TAG, "%s", msg.c_str());
        if (_on_log) {
            _on_log(msg);
        }
    }

    bool start_ap()
    {
        static esp_netif_t* ap_netif = nullptr;
        if (ap_netif == nullptr) {
            ap_netif = esp_netif_create_default_wifi_ap();
        }
        if (ap_netif == nullptr) {
            log("Failed to create AP interface");
            return false;
        }

        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_set_ip_info(ap_netif, &ip_info);
        esp_netif_dhcps_start(ap_netif);

        _dns_server = std::make_unique<DnsServer>();
        _dns_server->Start(ip_info.gw);
        _ssid = make_ap_ssid();

        wifi_config_t wifi_config = {};
        strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), _ssid.c_str(), sizeof(wifi_config.ap.ssid) - 1);
        wifi_config.ap.ssid_len = _ssid.size();
        wifi_config.ap.max_connection = 4;
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;

        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED && ret != ESP_ERR_WIFI_MODE) {
            ESP_LOGW(TAG, "wifi stop before AP failed: %s", esp_err_to_name(ret));
        }
        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) {
            log("Failed to set APSTA mode");
            return false;
        }
        ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
        if (ret != ESP_OK) {
            log("Failed to apply AP config");
            return false;
        }
        ret = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "disable power save failed: %s", esp_err_to_name(ret));
        }
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            log("Failed to start AP");
            return false;
        }
        return true;
    }

    bool start_server()
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers = 20;
        config.uri_match_fn = httpd_uri_match_wildcard;
        config.recv_wait_timeout = 15;
        config.send_wait_timeout = 15;
        esp_err_t ret = httpd_start(&_server, &config);
        if (ret != ESP_OK) {
            log("Failed to start web server");
            return false;
        }
        httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = &Session::handle_index, .user_ctx = this};
        httpd_uri_t css = {.uri = "/configure_ap.css", .method = HTTP_GET, .handler = &Session::handle_css, .user_ctx = this};
        httpd_uri_t js = {.uri = "/configure_ap.js", .method = HTTP_GET, .handler = &Session::handle_js, .user_ctx = this};
        httpd_uri_t config_get = {.uri = "/config", .method = HTTP_GET, .handler = &Session::handle_config_get, .user_ctx = this};
        httpd_uri_t config_post = {.uri = "/config", .method = HTTP_POST, .handler = &Session::handle_config_post, .user_ctx = this};
        httpd_uri_t mqtt_test = {.uri = "/mqtt/test", .method = HTTP_POST, .handler = &Session::handle_mqtt_test, .user_ctx = this};
        httpd_uri_t scan = {.uri = "/wifi/scan", .method = HTTP_GET, .handler = &Session::handle_wifi_scan, .user_ctx = this};
        httpd_uri_t close = {.uri = "/close", .method = HTTP_POST, .handler = &Session::handle_close, .user_ctx = this};
        httpd_uri_t reboot = {.uri = "/reboot", .method = HTTP_POST, .handler = &Session::handle_reboot, .user_ctx = this};
        httpd_uri_t captive = {.uri = nullptr, .method = HTTP_GET, .handler = &Session::handle_captive, .user_ctx = this};
        ret = httpd_register_uri_handler(_server, &index);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &css);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &js);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &config_get);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &config_post);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &mqtt_test);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &scan);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &close);
        if (ret == ESP_OK) ret = httpd_register_uri_handler(_server, &reboot);
        if (ret == ESP_OK) {
            for (const auto* url : CAPTIVE_URLS) {
                captive.uri = url;
                ret = httpd_register_uri_handler(_server, &captive);
                if (ret != ESP_OK) break;
            }
        }
        if (ret != ESP_OK) {
            log("Failed to register routes");
            return false;
        }
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            if (g_active_event_group == _event_group) {
                g_active_event_group = nullptr;
            }
        }
        if (_server) {
            httpd_stop(_server);
            _server = nullptr;
        }
        if (_dns_server) {
            _dns_server->Stop();
            _dns_server.reset();
        }
        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED && ret != ESP_ERR_WIFI_MODE) {
            ESP_LOGW(TAG, "wifi stop failed: %s", esp_err_to_name(ret));
        }
        if (_event_group) {
            vEventGroupDelete(_event_group);
            _event_group = nullptr;
        }
    }

    static Session* self(httpd_req_t* req) { return static_cast<Session*>(req->user_ctx); }

    static void send_json(httpd_req_t* req, const std::string& body)
    {
        httpd_resp_set_type(req, JSON_TYPE);
        httpd_resp_send(req, body.c_str(), body.size());
    }

    static std::string read_body(httpd_req_t* req)
    {
        std::string body(static_cast<size_t>(req->content_len), '\0');
        size_t offset = 0;
        while (offset < body.size()) {
            int got = httpd_req_recv(req, body.data() + offset, body.size() - offset);
            if (got <= 0) {
                return "";
            }
            offset += static_cast<size_t>(got);
        }
        return body;
    }

    static esp_err_t handle_index(httpd_req_t* req)
    {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req,
                        configure_ap_html_start,
                        configure_ap_html_end - configure_ap_html_start);
        return ESP_OK;
    }

    static esp_err_t handle_css(httpd_req_t* req)
    {
        httpd_resp_set_type(req, CSS_TYPE);
        httpd_resp_send(req,
                        configure_ap_css_start,
                        configure_ap_css_end - configure_ap_css_start);
        return ESP_OK;
    }

    static esp_err_t handle_js(httpd_req_t* req)
    {
        httpd_resp_set_type(req, JS_TYPE);
        httpd_resp_send(req,
                        configure_ap_js_start,
                        configure_ap_js_end - configure_ap_js_start);
        return ESP_OK;
    }

    static esp_err_t handle_config_get(httpd_req_t* req)
    {
        auto* s = self(req);
        send_json(req, config_to_json(device_config::load(), s ? s->_ssid : ""));
        return ESP_OK;
    }

    static esp_err_t handle_config_post(httpd_req_t* req)
    {
        std::string body = read_body(req);
        if (body.empty() && req->content_len > 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
            return ESP_FAIL;
        }

        device_config::Config cfg;
        cfg.device_name = get_json_string(body, "device_name");
        cfg.wifi_ssid = get_json_string(body, "wifi_ssid");
        cfg.wifi_password = get_json_string(body, "wifi_password");
        cfg.mqtt_uri = get_json_string(body, "mqtt_uri");
        cfg.mqtt_username = get_json_string(body, "mqtt_username");
        cfg.mqtt_password = get_json_string(body, "mqtt_password");
        cfg.counter_topic = get_json_string(body, "counter_topic");
        auto defaults = device_config::defaults();
        if (cfg.device_name.empty()) cfg.device_name = defaults.device_name;
        if (cfg.mqtt_uri.empty()) cfg.mqtt_uri = defaults.mqtt_uri;
        if (cfg.counter_topic.empty()) cfg.counter_topic = defaults.counter_topic;

        if (!device_config::save(cfg)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
            return ESP_FAIL;
        }
        httpd_resp_sendstr(req, "Saved. Changes will take effect after reboot.");
        return ESP_OK;
    }

    static esp_err_t handle_mqtt_test(httpd_req_t* req)
    {
        std::string body = read_body(req);
        if (body.empty() && req->content_len > 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
            return ESP_FAIL;
        }

        std::string uri = get_json_string(body, "mqtt_uri");
        std::string username = get_json_string(body, "mqtt_username");
        std::string password = get_json_string(body, "mqtt_password");
        if (uri.empty()) {
            httpd_resp_sendstr(req, "MQTT test failed: broker URI is empty.");
            return ESP_OK;
        }

        MqttTestContext ctx;
        ctx.event_group = xEventGroupCreate();
        if (ctx.event_group == nullptr) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "test allocation failed");
            return ESP_FAIL;
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
            httpd_resp_sendstr(req, "MQTT test failed: could not create test client.");
            return ESP_OK;
        }

        esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_test_event_handler, &ctx);
        esp_err_t err = esp_mqtt_client_start(client);
        if (err != ESP_OK) {
            esp_mqtt_client_destroy(client);
            vEventGroupDelete(ctx.event_group);
            httpd_resp_sendstr(req, "MQTT test failed: could not start test client.");
            return ESP_OK;
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
            httpd_resp_sendstr(req, "MQTT connection successful.");
        } else if (bits & MQTT_TEST_ERROR_BIT) {
            httpd_resp_sendstr(req, "MQTT test failed: connection error, authentication failure, or broker refused connection.");
        } else {
            httpd_resp_sendstr(req, "MQTT test failed: timeout. Confirm the broker address, port, and network path.");
        }
        return ESP_OK;
    }

    static esp_err_t handle_wifi_scan(httpd_req_t* req)
    {
        uint16_t ap_count = 0;
        wifi_scan_config_t scan_config = {};
        esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
        if (ret != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
            return ESP_FAIL;
        }
        esp_wifi_scan_get_ap_num(&ap_count);
        std::vector<wifi_ap_record_t> records(ap_count);
        if (ap_count > 0) {
            esp_wifi_scan_get_ap_records(&ap_count, records.data());
        }
        std::string json = "{\"networks\":[";
        for (uint16_t i = 0; i < ap_count; ++i) {
            if (i) json += ",";
            json += "{\"ssid\":\"" + json_escape(reinterpret_cast<const char*>(records[i].ssid)) + "\",";
            json += "\"rssi\":" + std::to_string(records[i].rssi) + "}";
        }
        json += "]}";
        send_json(req, json);
        return ESP_OK;
    }

    static esp_err_t handle_close(httpd_req_t* req)
    {
        auto* s = self(req);
        if (s && s->_event_group) {
            xEventGroupSetBits(s->_event_group, EXIT_BIT);
        }
        httpd_resp_sendstr(req, "closing");
        return ESP_OK;
    }

    static esp_err_t handle_reboot(httpd_req_t* req)
    {
        httpd_resp_sendstr(req, "rebooting");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return ESP_OK;
    }

    static esp_err_t handle_captive(httpd_req_t* req)
    {
        std::string url = std::string(AP_URL) + "/?_=" + std::to_string(esp_timer_get_time());
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", url.c_str());
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    httpd_handle_t _server = nullptr;
    EventGroupHandle_t _event_group = nullptr;
    std::function<void(std::string_view)> _on_log;
    std::string _ssid;
    std::unique_ptr<DnsServer> _dns_server;
};

}  // namespace

bool run(const std::function<void(std::string_view)>& onLog)
{
    return Session(onLog).run();
}

void requestStop()
{
    std::lock_guard<std::mutex> lock(g_session_mutex);
    if (g_active_event_group != nullptr) {
        xEventGroupSetBits(g_active_event_group, EXIT_BIT);
    }
}

}  // namespace configure_ap
