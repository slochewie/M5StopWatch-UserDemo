#pragma once

#include <cstdint>
#include <string>

namespace device_config {

struct Config {
    std::string device_name = "Capacity-01";
    std::string wifi_ssid;
    std::string wifi_password;
    std::string mqtt_uri = "mqtt://smbhub.local:1883";
    std::string mqtt_username;
    std::string mqtt_password;
    std::string counter_topic = "counters/capacity/state";
};

Config defaults();
Config load();
bool save(const Config& config);
void reset();

}  // namespace device_config
