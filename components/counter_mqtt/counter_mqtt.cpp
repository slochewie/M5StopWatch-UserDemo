#include "counter_mqtt.h"

#include <counter_service.h>

namespace counter_mqtt {

void begin()
{
    counter_service::begin();
}

bool isStarted()
{
    return counter_service::isStarted();
}

bool isConnected()
{
    return counter_service::isConnected();
}

bool publishCounterValue(int32_t value)
{
    return counter_service::publishValue(value);
}

bool publishBatteryPercentage(uint8_t percent)
{
    return counter_service::publishBatteryPercentage(percent);
}

bool takeLatestValue(int32_t& value)
{
    return counter_service::takeLatestValue(value);
}

const char* statusText()
{
    return counter_service::statusText();
}

const char* brokerUri()
{
    return counter_service::brokerUri();
}

const char* counterTopic()
{
    return counter_service::counterTopic();
}

const char* batteryTopic()
{
    return counter_service::batteryTopic();
}

const char* deviceName()
{
    return counter_service::deviceName();
}

const char* wifiSsid()
{
    return counter_service::wifiSsid();
}

}  // namespace counter_mqtt
