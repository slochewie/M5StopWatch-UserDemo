#include "counter_mqtt.h"

#include <apps/common/network/mqtt_service.h>
#include <apps/common/network/wifi_service.h>

namespace counter_mqtt {

void recoverConnection()
{
    common::wifi::recoverConnection();

    if (!common::mqtt::isStarted()) {
        begin();
        return;
    }

    common::mqtt::recoverConnection();
}

}  // namespace counter_mqtt
