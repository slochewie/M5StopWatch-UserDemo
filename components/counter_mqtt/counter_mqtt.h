#pragma once

#include <cstdint>

namespace counter_mqtt {

void begin();
bool isStarted();
bool isConnected();
bool takeLatestValue(int32_t& value);
const char* statusText();

}  // namespace counter_mqtt
