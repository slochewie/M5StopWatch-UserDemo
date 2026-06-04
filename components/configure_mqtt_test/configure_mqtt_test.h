#pragma once

#include <string>

namespace configure_mqtt_test {

std::string testConnection(const std::string& uri,
                           const std::string& username,
                           const std::string& password);

}  // namespace configure_mqtt_test
