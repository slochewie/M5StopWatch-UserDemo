#pragma once

#include <cstdint>

namespace counter_service {

void begin();
void recoverConnection();
bool isStarted();
bool isConnected();
bool publishValue(int32_t value);
bool publishBatteryPercentage(uint8_t percent);
bool takeLatestValue(int32_t& value);
const char* statusText();
const char* brokerUri();
const char* counterTopic();
const char* batteryTopic();
const char* deviceName();
const char* wifiSsid();

}  // namespace counter_service
