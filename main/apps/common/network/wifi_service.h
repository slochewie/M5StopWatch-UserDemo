#pragma once

#include <string>

namespace common::wifi {

struct Config {
    std::string ssid;
    std::string password;
};

bool begin(const Config& config);
void recoverConnection();
void setRecoveryPaused(bool paused);
bool isRecoveryPaused();
bool isStarted();
bool isConnected();
const char* ssid();
const char* statusText();

}  // namespace common::wifi
