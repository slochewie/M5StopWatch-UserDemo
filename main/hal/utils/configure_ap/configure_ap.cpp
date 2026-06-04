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

std::mutex g_session_mutex;
EventGroupHandle_t g_active_event_group = nullptr;

constexpr const char* CAPTIVE_URLS[] = {
    "/hotspot-detect.html", "/generate_204*", "/mobile/status.php", "/check_network_status.txt",
    "/ncsi.txt", "/fwlink/", "/connectivity-check.html", "/success.txt", "/portal.html",
    "/library/test/success.html",
};

const char INDEX_HTML[] = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>M5 Configure</title><style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;background:#090909;color:#f5f5f7;margin:0;padding:24px}main{max-width:620px;margin:auto}.card{background:#171717;border:1px solid #333;border-radius:18px;padding:18px;margin:14px 0}label{display:block;color:#bbb;margin:12px 0 5px}input,select{width:100%;box-sizing:border-box;background:#0b0b0b;color:white;border:1px solid #444;border-radius:12px;padding:12px;font-size:16px}button{border:0;border-radius:12px;background:#0a84ff;color:white;padding:12px 14px;margin:8px 8px 0 0;font-weight:700}.secondary{background:#333}.danger{background:#ff453a}.status{white-space:pre-wrap;color:#a7f0a7}.toggleRow{display:flex;align-items:center;justify-content:space-between;gap:14px;margin:12px 0}.switch{position:relative;display:inline-block;width:58px;height:32px}.switch input{display:none}.slider{position:absolute;cursor:pointer;inset:0;background:#444;border-radius:999px;transition:.2s}.slider:before{content:"";position:absolute;height:24px;width:24px;left:4px;top:4px;background:white;border-radius:50%;transition:.2s}input:checked+.slider{background:#0a84ff}input:checked+.slider:before{transform:translateX(26px)}.hint{font-size:13px;color:#999;margin-top:6px}.preview{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;background:#0b0b0b;border:1px solid #333;border-radius:12px;padding:10px;margin-top:10px;color:#ddd;overflow-wrap:anywhere}.hidden{display:none}.saved{border-color:#236d32}</style></head><body><main><div class="card"><h1>M5StopWatch Configure</h1><p>Configure network, MQTT, device name, and counter topic.</p><div id="status" class="status">Loading...</div></div><form id="form" class="card"><h2>Wi-Fi</h2><button type="button" class="secondary" id="scan">Scan</button><label>Nearby SSID</label><select id="ssid_select"><option value="">Scan first</option></select><label>Wi-Fi Network (SSID)</label><input id="wifi_ssid"><div class="hint">Select a scanned network or enter one manually.</div><label>Password</label><input id="wifi_password" type="password"><h2>MQTT</h2><div class="toggleRow"><div><b>Encryption</b><div class="hint">Off = mqtt://, On = mqtts://</div></div><label class="switch"><input id="mqtt_encrypt" type="checkbox"><span class="slider"></span></label></div><label>Broker IP or Domain</label><input id="mqtt_host" placeholder="smbhub.local or 192.168.75.61"><label>Port</label><input id="mqtt_port" inputmode="numeric" pattern="[0-9]*" value="1883"><div class="hint">Full URI preview</div><div id="mqtt_preview" class="preview">mqtt://:1883</div><label>MQTT Username</label><input id="mqtt_username"><label>MQTT Password</label><input id="mqtt_password" type="password"><div class="hint">Used to authenticate with your MQTT broker.</div><button type="button" class="secondary" id="mqtt_test">Test MQTT Connection</button><h2>Counter</h2><label>Device Name</label><input id="device_name"><div class="hint">Used as the device identifier and MQTT client name.</div><label>Counter Topic</label><input id="counter_topic"><div class="hint">Example: counters/capacity/state</div><button type="submit">Save</button><button type="button" class="secondary" id="reload">Reload</button><button type="button" class="secondary" id="close">Close Portal</button></form><div id="saved_card" class="card saved hidden"><h2>Settings Saved</h2><p>Changes will take effect after reboot.</p><button type="button" class="danger" id="reboot">Reboot Device</button></div></main><script>const ids=['device_name','wifi_ssid','wifi_password','mqtt_username','mqtt_password','counter_topic'];const el=id=>document.getElementById(id);function st(s){el('status').textContent=s}function mqttUri(){const prefix=el('mqtt_encrypt').checked?'mqtts://':'mqtt://';const host=el('mqtt_host').value.trim();const port=(el('mqtt_port').value.trim()||'1883');return prefix+host+':'+port}function updatePreview(){el('mqtt_preview').textContent=mqttUri()}function parseMqttUri(uri){let u=uri||'';let secure=false;if(u.startsWith('mqtts://')){secure=true;u=u.slice(8)}else if(u.startsWith('mqtt://')){u=u.slice(7)}let host=u;let port='1883';const slash=host.indexOf('/');if(slash>=0)host=host.slice(0,slash);const colon=host.lastIndexOf(':');if(colon>0){port=host.slice(colon+1)||'1883';host=host.slice(0,colon)}el('mqtt_encrypt').checked=secure;el('mqtt_host').value=host;el('mqtt_port').value=port||'1883';updatePreview()}function payload(){const b={};ids.forEach(k=>b[k]=el(k).value);b.mqtt_uri=mqttUri();return b}function showSaved(){el('saved_card').classList.remove('hidden')}async function load(){const r=await fetch('/config');const c=await r.json();ids.forEach(k=>{if(c[k]!==undefined)el(k).value=c[k]});parseMqttUri(c.mqtt_uri||'');st('AP: '+(c.ap_ssid||'')+'\nURL: '+(c.ap_url||''))}async function save(e){e.preventDefault();const r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload())});st(await r.text());showSaved()}async function testMqtt(){st('Testing MQTT connection...');const r=await fetch('/mqtt/test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload())});st(await r.text())}async function scan(){st('Scanning...');const r=await fetch('/wifi/scan');const j=await r.json();const s=el('ssid_select');s.innerHTML='';j.networks.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' RSSI '+n.rssi;s.appendChild(o)});st('Scan complete')}async function reboot(){st('Rebooting device...');await fetch('/reboot',{method:'POST'})}el('form').addEventListener('submit',save);el('reload').addEventListener('click',load);el('scan').addEventListener('click',scan);el('mqtt_test').addEventListener('click',testMqtt);el('reboot').addEventListener('click',reboot);el('ssid_select').addEventListener('change',()=>{if(el('ssid_select').value)el('wifi_ssid').value=el('ssid_select').value});el('close').addEventListener('click',async()=>{await fetch('/close',{method:'POST'});st('Portal closing')});['mqtt_encrypt','mqtt_host','mqtt_port'].forEach(id=>el(id).addEventListener('input',updatePreview));['mqtt_encrypt','mqtt_host','mqtt_port'].forEach(id=>el(id).addEventListener('change',updatePreview));load().catch(e=>st('Load failed: '+e));</script></body></html>)HTML";

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
        httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
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
