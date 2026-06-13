#pragma once

#include <cstdint>
#include <string>

namespace common::mqtt {

using MessageCallback = void (*)(const char* topic, const char* payload, void* user_data);

struct Config {
    std::string uri;
    std::string client_id;
    std::string username;
    std::string password;
};

bool begin(const Config& config);
void recoverConnection();
void setRecoveryPaused(bool paused);
bool isRecoveryPaused();
bool isStarted();
bool isConnected();
bool subscribe(const char* topic, int qos, MessageCallback callback, void* user_data = nullptr);
bool publish(const char* topic, const char* payload, int qos = 1, bool retain = true);
const char* brokerUri();
const char* statusText();

}  // namespace common::mqtt
